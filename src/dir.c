#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

#include "info.h"
#include "type.h"
#include "opts.h"
#include "dir.h"
#include "io.h"


char * cwd = NULL;
char * homedir = NULL;
size_t homedir_len = 0;
bool in_home_subdir = false;

size_t n_dir_entries = 0;
size_t dir_longest_owner = 0;
size_t dir_longest_group = 0;
struct dir_entry_t ** dir_entries = NULL;

bool permission_denied = false;
bool cwd_is_file = false;


static int cmp(const void * a, const void * b) {
	const struct dir_entry_t * c = *(const struct dir_entry_t **)a;
	const struct dir_entry_t * d = *(const struct dir_entry_t **)b;

	// sort directories first
	if (c->file_type == FILE_DIR && d->file_type != FILE_DIR)
		return -1;
	else if (d->file_type == FILE_DIR && c->file_type != FILE_DIR)
		return 1;
	else
		return strcasecmp(c->name, d->name);
}

static int acmp(const void * a, const void * b) {
	return strcasecmp((*(const struct dir_entry_t**)a)->name,
			(*(const struct dir_entry_t**)b)->name);
}


struct dir_entry_t * gen_dir_entry(char * dir_path, char * d_name) {
	struct dir_entry_t * dir_entry = malloc(sizeof(struct dir_entry_t) + 12);

	size_t d_name_len = strlen(d_name);

	struct stat * buf = malloc(sizeof(struct stat));
	char * tmp_path = malloc(d_name_len + strlen(dir_path) + 2);
	sprintf(tmp_path, "%s/%s", dir_path, d_name);
	if (lstat(tmp_path, buf) == -1) {
		free(dir_entry);
		free(buf);
		free(tmp_path);

		return NULL;
	}

	dir_entry->name = malloc(d_name_len + 1);
	strcpy(dir_entry->name, d_name);

	dir_entry->file_type = FILE_UNKNOWN;
	dir_entry->under_link = FILE_UNKNOWN;
	switch(buf->st_mode & S_IFMT) {
		case S_IFBLK:
		case S_IFCHR:
			dir_entry->file_type = FILE_BLK;
			break;
		case S_IFSOCK:
			dir_entry->file_type = FILE_SOCK;
			break;
		case S_IFDIR:
			dir_entry->file_type = FILE_DIR;
			break;
		case S_IFLNK:
			dir_entry->file_type = FILE_LINK;
			struct stat * buf2 = malloc(sizeof(struct stat));
			stat(tmp_path, buf2);
			if ((buf2->st_mode & S_IFMT) == S_IFDIR)
				dir_entry->under_link = FILE_DIR;
			free(buf2);
			break;
		case S_IFIFO:
			dir_entry->file_type = FILE_FIFO;
			break;
		case S_IFREG:
			dir_entry->file_type = FILE_REG;
			break;
		default:
			dir_entry->file_type = FILE_UNKNOWN;
			break;
	}

	dir_entry->mode = 0;

	// other mode
	if (buf->st_mode & S_IROTH)
		dir_entry->mode |= M_READ;
	if (buf->st_mode & S_IWOTH)
		dir_entry->mode |= M_WRITE;
	if (buf->st_mode & S_IXOTH)
		dir_entry->mode |= M_EXEC;
	if (buf->st_mode & S_ISVTX) // sticky
		dir_entry->mode |= M_SUID;

	// group mode
	if (buf->st_mode & S_IRGRP)
		dir_entry->mode |= PGROUP(M_READ);
	if (buf->st_mode & S_IWGRP)
		dir_entry->mode |= PGROUP(M_WRITE);
	if (buf->st_mode & S_IXGRP)
		dir_entry->mode |= PGROUP(M_EXEC);
	if (buf->st_mode & S_ISGID) // suid; group
		dir_entry->mode |= PGROUP(M_SUID);

	// owner mode
	if (buf->st_mode & S_IRUSR)
		dir_entry->mode |= POWNER(M_READ);
	if (buf->st_mode & S_IWUSR)
		dir_entry->mode |= POWNER(M_WRITE);
	if (buf->st_mode & S_IXUSR)
		dir_entry->mode |= POWNER(M_EXEC);
	if (buf->st_mode & S_ISUID) // suid
		dir_entry->mode |= POWNER(M_SUID);


#if defined(__APPLE__) || defined(__MACH__)
	dir_entry->mtime = buf->st_mtime;
#else
	dir_entry->mtime = buf->st_mtim.tv_sec;
#endif

	dir_entry->owner = buf->st_uid;
	dir_entry->group = buf->st_gid;

	struct passwd * pw = getpwuid(buf->st_uid);
	size_t pw_l = 0;
	if (!pw) pw_l = get_ilen(buf->st_uid, 10);
	else pw_l = strlen(pw->pw_name);

	struct group * gr = getgrgid(buf->st_gid);
	size_t gr_l = 0;
	if (!gr) gr_l = get_ilen(buf->st_gid, 10);
	else gr_l = strlen(gr->gr_name);

	if (pw_l > dir_longest_owner) dir_longest_owner = pw_l;
	if (gr_l > dir_longest_group) dir_longest_group = gr_l;

	dir_entry->size = buf->st_size;
	dir_entry->u_size = B;

	for (int i = 0; i <= PB; i++) {
		if (dir_entry->size < 1000) break;
		dir_entry->size /= 1000;
		dir_entry->u_size++;
	}

	free(tmp_path);
	free(buf);

	// figure out file 'mime' type
	dir_entry->m_type = get_mime(dir_entry->name);

	dir_entry->marked = false;
	dir_entry->last_in_col = false;

	return dir_entry;
}


int list_dir(char * dir_path) {
	struct dirent * d_entry;
	DIR * dir = opendir(dir_path);

	n_dir_entries = 0;

	if (!dir) {
		if (errno == EACCES)
			permission_denied = true;
		return 1;
	}
	permission_denied = false;

	while ((d_entry = readdir(dir))) {
		char * d_name = d_entry->d_name;

		if ((!strcmp(d_name, ".") || !strcmp(d_name, ".."))
			&& !show_dot_dirs) {
			continue;
		} else if (!show_dot_files && d_name[0] == '.') {
			continue;
		}

		struct dir_entry_t * dir_entry = gen_dir_entry(dir_path, d_name);

		if (!dir_entry) {
			display_info(INFO_WARN, strerror(errno));
			continue;
		}

		dir_entries = realloc(dir_entries, sizeof(struct dir_entry_t*) * (n_dir_entries + 1));
		dir_entries[n_dir_entries] = dir_entry;
		n_dir_entries++;
	}

	closedir(dir);

	qsort(dir_entries, n_dir_entries, sizeof(struct dir_entry_t*), cmp);
	size_t d_end = 0;
	for (size_t i = 0; i < n_dir_entries; i++) {
		if (dir_entries[i]->file_type != FILE_DIR) {
			d_end = i;
			break;
		}
	}
	qsort(dir_entries, d_end, sizeof(struct dir_entry_t*), acmp);
	qsort(dir_entries + d_end, n_dir_entries - d_end,
			sizeof(struct dir_entry_t*), acmp);


	return 0;
}


void free_dir_entries(void) {
	for (size_t i = 0; i < n_dir_entries; i++) {
		free(dir_entries[i]->name);
		free(dir_entries[i]);
	}
	
	n_dir_entries = 0;
}


void cd_back(void) {
	char * p = strrchr(cwd, '/');
	*p = '\0';
	cwd = realloc(cwd, strlen(cwd) + 2);
	if (cwd[0] == '\0') {
		cwd[0] = '/';
		cwd[1] = '\0';
	}
	chdir(cwd);

	dir_longest_owner = 0;
	dir_longest_group = 0;

	if (!strncmp(cwd, homedir, homedir_len)) in_home_subdir = true;
	else in_home_subdir = false;
}


void enter_dir(char * name) {
	char * tmp = malloc(strlen(cwd) + strlen(name) + 2);

	if (strcmp(cwd, "/"))
		sprintf(tmp, "%s/%s", cwd, name);
	else
		sprintf(tmp, "%s%s", cwd, name);

	if (chdir(tmp) == -1) {
		display_info(INFO_ERR, strerror(errno));
	} else {
		free(cwd);
		cwd = tmp;

		dir_longest_owner = 0;
		dir_longest_group = 0;

		if (!strncmp(cwd, homedir, homedir_len)) in_home_subdir = true;
		else in_home_subdir = false;
	}
}


void remove_marked(void) {
	for (size_t i = 0; i < n_dir_entries; i++) {
		if (dir_entries[i]->marked) {
			char p[strlen(cwd) + strlen(dir_entries[i]->name) + 1];
			sprintf(p, "%s/%s", cwd, dir_entries[i]->name);
			if (remove(p) == 0) n_marked_files--;
		}
	}
}


char * mode_to_s(struct dir_entry_t * f) {
	char * s = malloc(11);
	char * p = s;
	uint16_t mode = f->mode;

	if (f->file_type == FILE_DIR) *p++ = 'd';
	else *p++ = '.';

	// owner mode
	if (MOWNER(mode) & M_READ) *p++ = 'r';
	else *p++ = '-';
	if (MOWNER(mode) & M_WRITE) *p++ = 'w';
	else *p++ = '-';
	if (MOWNER(mode) & M_EXEC) {
		if (MOWNER(mode) & M_SUID) *p++ = 's';
		else *p++ = 'x';
	} else if (MOWNER(mode) & M_SUID) *p++ = 'S';
	else *p++ = '-';

	// group mode
	if (MGROUP(mode) & M_READ) *p++ = 'r';
	else *p++ = '-';
	if (MGROUP(mode) & M_WRITE) *p++ = 'w';
	else *p++ = '-';
	if (MGROUP(mode) & M_EXEC) {
		if (MGROUP(mode) & M_SUID) *p++ = 's';
		else *p++ = 'x';
	} else if (MGROUP(mode) & M_SUID) *p++ = 'S';
	else *p++ = '-';

	// other mode
	if (mode & M_READ) *p++ = 'r';
	else *p++ = '-';
	if (mode & M_WRITE) *p++ = 'w';
	else *p++ = '-';
	if (mode & M_EXEC) {
		if (mode & M_SUID) *p++ = 't';
		else *p++ = 'x';
	} else if (mode & M_SUID) *p++ = 'T';
	else *p++ = '-';

	*p++ = 0;

	return s;
}


// check if path exists & is a dir
bool check_dpath(char * s) {
	struct stat buf;
	if (stat(s, &buf) == -1) return false;
	if ((buf.st_mode & S_IFMT) == S_IFDIR) return true;
	return false;
}


void get_home(void) {
	char * s = getenv("HOME");
	// check if var exists & is a real dir
	if (!s || *s == '\0' || !check_dpath(s)) {
		struct passwd * pw = getpwuid(geteuid());
		s = pw->pw_dir;
	}

	homedir = realpath(s, NULL);
	homedir_len = strlen(homedir);
}
