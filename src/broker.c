// SPDX-License-Identifier: GPL-2.0-or-later
#include "broker.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

void broker_init(struct broker *b, const struct manifest *m, const char *home)
{
	memset(b, 0, sizeof *b);
	b->m = m;
	if (!home)
		home = getenv("HOME");
	snprintf(b->home, sizeof b->home, "%s", home ? home : "");
	/* drop trailing slashes so "~/x" never becomes "//x" */
	size_t n = strlen(b->home);
	while (n > 1 && b->home[n - 1] == '/')
		b->home[--n] = 0;
}

void broker_free(struct broker *b)
{
	for (size_t i = 0; i < b->nfiles; i++)
		free(b->files[i]);
	b->nfiles = 0;
}

static bool lexical_ok(const char *p, bool allow_trailing_slash)
{
	if (p[0] != '/')
		return false;
	size_t n = strlen(p);
	const char *c = p + 1, *end = p + n;
	if (allow_trailing_slash && n > 1 && p[n - 1] == '/')
		end--;
	if (c >= end)
		return false;
	while (c < end) {
		const char *s = memchr(c, '/', (size_t)(end - c));
		size_t len = s ? (size_t)(s - c) : (size_t)(end - c);
		if (len == 0 || (len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.'))
			return false;
		c += len + 1;
	}
	return true;
}

static bool expand(const char *home, const char *req, char *out, size_t outlen, bool dir)
{
	size_t n = strlen(req);
	if (n == 0 || n > MAX_PATH_LEN || !utf8_valid(req, n))
		return false;
	int w;
	if (req[0] == '~' && req[1] == '/') {
		if (home[0] != '/')
			return false;
		w = snprintf(out, outlen, "%s%s", strcmp(home, "/") ? home : "", req + 1);
	} else if (req[0] == '/') {
		w = snprintf(out, outlen, "%s", req);
	} else {
		return false;
	}
	if (w < 0 || (size_t)w >= outlen)
		return false;
	if (dir && !strcmp(out, "/"))
		return true; /* "~/" with HOME=/ */
	return lexical_ok(out, dir);
}

bool broker_expand(const struct broker *b, const char *req, char *out, size_t outlen)
{
	if (req[0] && req[strlen(req) - 1] == '/')
		return false;
	return expand(b->home, req, out, outlen, false);
}

/* Finds the read covering abs; writes the directory to open from and the path beneath it. */
static bool cover(const struct broker *b, const char *abs, char *root, size_t rootlen, const char **rel)
{
	char e[MAX_PATH_LEN + 1100];
	for (size_t i = 0; i < b->m->nreads; i++) {
		const struct read_entry *r = &b->m->reads[i];
		if (!expand(b->home, r->path, e, sizeof e, r->dir))
			continue;
		if (r->dir) {
			size_t n = strlen(e);
			if (e[n - 1] != '/' && n + 1 < sizeof e) {
				e[n++] = '/';
				e[n] = 0;
			}
			if (!strncmp(abs, e, n) && abs[n]) {
				snprintf(root, rootlen, "%.*s", (int)(n > 1 ? n - 1 : n), e);
				*rel = abs + n;
				return true;
			}
		} else if (!strcmp(abs, e)) {
			const char *slash = strrchr(abs, '/');
			size_t dn = (size_t)(slash - abs);
			snprintf(root, rootlen, "%.*s", (int)(dn ? dn : 1), abs);
			*rel = slash + 1;
			return true;
		}
	}
	return false;
}

bool broker_allowed(const struct broker *b, const char *abs)
{
	char root[4096];
	const char *rel;
	return lexical_ok(abs, false) && cover(b, abs, root, sizeof root, &rel);
}

bool broker_path_check(void *broker, const char *path)
{
	return broker_allowed(broker, path);
}

static int open_beneath_walk(int dirfd, const char *rel)
{
	char comp[256];
	int fd = dup(dirfd);
	if (fd < 0)
		return -1;
	for (;;) {
		const char *s = strchr(rel, '/');
		size_t n = s ? (size_t)(s - rel) : strlen(rel);
		if (n >= sizeof comp) {
			close(fd);
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(comp, rel, n);
		comp[n] = 0;
		int flags = s ? O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
			      : O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC;
		int nfd = openat(fd, comp, flags);
		int e = errno;
		if (nfd < 0 && e == ENOTDIR) {
			/* a symlink opened with O_PATH|O_NOFOLLOW is not a directory */
			struct stat st;
			if (!fstatat(fd, comp, &st, AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode))
				e = ELOOP;
		}
		close(fd);
		if (nfd < 0) {
			errno = e == ELOOP ? EACCES : e;
			return -1;
		}
		fd = nfd;
		if (!s)
			return fd;
		rel = s + 1;
	}
}

static int open_beneath(int dirfd, const char *rel, bool force_fallback)
{
	if (!force_fallback) {
		struct open_how how = {
			.flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC,
			.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS,
		};
		int fd = (int)syscall(SYS_openat2, dirfd, rel, &how, sizeof how);
		if (fd >= 0)
			return fd;
		if (errno != ENOSYS && errno != EPERM) {
			if (errno == ELOOP || errno == EXDEV)
				errno = EACCES;
			return -1;
		}
		/* no openat2 (old kernel, or a container filter): walk with O_NOFOLLOW */
	}
	return open_beneath_walk(dirfd, rel);
}

static void record(struct broker *b, const char *abs)
{
	for (size_t i = 0; i < b->nfiles; i++)
		if (!strcmp(b->files[i], abs))
			return;
	if (b->nfiles < BROKER_MAX_FILES) {
		char *p = strdup(abs);
		if (p)
			b->files[b->nfiles++] = p;
	}
}

int broker_open(struct broker *b, const char *req, long long *size)
{
	char abs[4096], root[4096];
	const char *rel;
	if (++b->requests > BROKER_MAX_FILES) {
		errno = EMFILE;
		return -1;
	}
	if (!broker_expand(b, req, abs, sizeof abs)) {
		errno = EINVAL;
		return -1;
	}
	if (!cover(b, abs, root, sizeof root, &rel)) {
		errno = EACCES;
		return -1;
	}
	record(b, abs);
	/* the read's own directory is the user's: symlinks up to it are followed */
	int dfd = open(root, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		return -1;
	int fd = open_beneath(dfd, rel, b->force_fallback);
	int e = errno;
	close(dfd);
	if (fd < 0) {
		errno = e;
		return -1;
	}
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	if (st.st_size > BROKER_MAX_FILE) {
		close(fd);
		errno = EFBIG;
		return -1;
	}
	*size = st.st_size;
	return fd;
}
