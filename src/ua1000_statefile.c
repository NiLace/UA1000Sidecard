/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ua1000_statefile.c - where the editor's state lives on disk
 */
#include "ua1000_statefile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE_DIR "ua1000"
#define STATE_NAME "state"

int ua1000_statefile_default_path(char *path, size_t len)
{
	const char *base = getenv("XDG_STATE_HOME");
	int n;

	/* The XDG specification ignores a relative path. */
	if (base && base[0] == '/') {
		n = snprintf(path, len, "%s/%s/%s", base, STATE_DIR,
			     STATE_NAME);
	} else {
		const char *home = getenv("HOME");

		if (!home || home[0] != '/')
			return -ENOENT; /* neither variable names a directory */
		n = snprintf(path, len, "%s/.local/state/%s/%s", home,
			     STATE_DIR, STATE_NAME);
	}
	return n < 0 || (size_t)n >= len ? -ENAMETOOLONG : 0;
}

int ua1000_statefile_load(const char *path, struct ua1000_state *state,
			  bool *existed, char *err, size_t err_len)
{
	FILE *f = fopen(path, "r");
	int ret;

	*existed = f != NULL;
	if (!f) {
		ua1000_state_init(state);
		if (errno == ENOENT)
			return 0;
		ret = -errno;
		snprintf(err, err_len, "%s: %s", path, strerror(-ret));
		return ret;
	}

	char why[160];
	struct stat sb;

	if (fstat(fileno(f), &sb) == 0 && S_ISDIR(sb.st_mode)) {
		fclose(f);
		ua1000_state_init(state);
		snprintf(err, err_len, "%s: is a directory", path);
		return -EISDIR;
	}
	ret = ua1000_state_read(state, f, why, sizeof(why));
	fclose(f);
	if (ret != 0)
		snprintf(err, err_len, "%s: %s", path, why);
	return ret;
}

/* Create every missing directory above path, like mkdir -p. */
static int make_parents(const char *path)
{
	char dir[4096];
	int n = snprintf(dir, sizeof(dir), "%s", path);

	if (n < 0 || (size_t)n >= sizeof(dir))
		return -ENAMETOOLONG;
	for (char *p = dir + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(dir, 0700) != 0 && errno != EEXIST)
			return -errno;
		*p = '/';
	}
	return 0;
}

/* Make a rename in path's directory durable. */
static int sync_parent(const char *path)
{
	char dir[4096];
	int n = snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	int fd, ret = 0;

	if (n < 0 || (size_t)n >= sizeof(dir) || !slash)
		return 0;
	*(slash == dir ? slash + 1 : slash) = '\0';
	fd = open(dir, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		return -errno;
	if (fsync(fd) != 0)
		ret = -errno;
	close(fd);
	return ret;
}

int ua1000_statefile_save(const char *path, const struct ua1000_state *state)
{
	char tmp[4096];
	int n = snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
	int ret = make_parents(path);

	if (ret != 0)
		return ret;
	if (n < 0 || (size_t)n >= sizeof(tmp))
		return -ENAMETOOLONG;

	int fd = mkstemp(tmp);

	if (fd < 0)
		return -errno;

	FILE *f = fdopen(fd, "w");

	if (!f) {
		ret = -errno;
		close(fd);
		unlink(tmp);
		return ret;
	}
	ret = ua1000_state_write(state, f);
	if (fflush(f) != 0 || fsync(fd) != 0)
		ret = -errno;
	if (fclose(f) != 0 && ret == 0)
		ret = -errno;
	if (ret == 0 && rename(tmp, path) != 0)
		ret = -errno;
	if (ret != 0) {
		unlink(tmp);
		return ret;
	}
	return sync_parent(path);
}
