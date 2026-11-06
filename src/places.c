// SPDX-License-Identifier: GPL-2.0-or-later
#include "places.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "toml.h"

#define DEFAULT_BOOKS "~/Books"
#define MAX_PLACES_FILE (16 * 1024)

bool place_known(const char *name, size_t len)
{
	return len == 5 && !strncmp(name, "books", 5);
}

bool places_path(const char *home, char *out, size_t len)
{
	const char *x = getenv("XDG_CONFIG_HOME");
	int n;
	if (home)
		n = snprintf(out, len, "%s/.config/glassine/places", home);
	else if (x && x[0] == '/')
		n = snprintf(out, len, "%s/glassine/places", x);
	else if ((home = getenv("HOME")) && home[0] == '/')
		n = snprintf(out, len, "%s/.config/glassine/places", home);
	else
		return false;
	return n > 0 && (size_t)n < len;
}

/* Expands and checks a place's value; false with a reason. */
static bool resolve(const char *v, const char *home, char *out, size_t outlen, char *why, size_t whylen)
{
	int n;
	if (!v[0]) {
		snprintf(why, whylen, "not set");
		return false;
	}
	if (v[0] == '~' && v[1] == '/') {
		if (!home || home[0] != '/') {
			snprintf(why, whylen, "no home directory");
			return false;
		}
		n = snprintf(out, outlen, "%s%s", strcmp(home, "/") ? home : "", v + 1);
	} else if (v[0] == '/') {
		n = snprintf(out, outlen, "%s", v);
	} else {
		snprintf(why, whylen, "\"%s\" is not absolute or ~/", v);
		return false;
	}
	if (n < 0 || (size_t)n >= outlen) {
		snprintf(why, whylen, "too long");
		return false;
	}
	/* drop one trailing slash: places name folders */
	size_t l = strlen(out);
	if (l > 1 && out[l - 1] == '/')
		out[--l] = 0;
	/* the same rules as a read: no . or .. parts, not / itself, not a system area */
	bool dir;
	if (!strcmp(out, "/") || !read_path_valid(out, &dir)) {
		snprintf(why, whylen, "\"%s\" is not an allowed folder", v);
		out[0] = 0;
		return false;
	}
	return true;
}

void places_load(struct places *p, const char *path, const char *home)
{
	memset(p, 0, sizeof *p);
	const char *books = DEFAULT_BOOKS;
	struct value *root = NULL;
	if (path) {
		int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
		if (fd >= 0) {
			struct stat st;
			char *text = NULL;
			ssize_t n = -1;
			if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size <= MAX_PLACES_FILE &&
			    (text = malloc((size_t)st.st_size + 1)))
				n = read(fd, text, (size_t)st.st_size);
			close(fd);
			char err[128];
			if (n == st.st_size)
				root = toml_parse(text, (size_t)n, err, sizeof err);
			free(text);
			if (!root) {
				snprintf(p->why, sizeof p->why, "the places file can't be read");
				return;
			}
			const struct value *b = v_map_get(root, "books");
			if (b && b->t != V_STR) {
				snprintf(p->why, sizeof p->why, "books is not a path");
				v_free(root);
				return;
			}
			if (b)
				books = b->u.s.p;
		} else if (errno != ENOENT) {
			snprintf(p->why, sizeof p->why, "the places file can't be read");
			return;
		}
	}
	resolve(books, home, p->books, sizeof p->books, p->why, sizeof p->why);
	v_free(root);
}

const char *places_get(const struct places *p, const char *name, size_t len)
{
	if (place_known(name, len) && p->books[0])
		return p->books;
	return NULL;
}
