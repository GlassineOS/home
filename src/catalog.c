// SPDX-License-Identifier: GPL-2.0-or-later
#include "catalog.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool catalog_user_root(char *out, size_t len)
{
	const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	int n;
	if (x && x[0] == '/')
		n = snprintf(out, len, "%s/glassine/widgets", x);
	else if (h && h[0] == '/')
		n = snprintf(out, len, "%s/.local/share/glassine/widgets", h);
	else
		return false;
	return n > 0 && (size_t)n < len;
}

static int by_ref(const void *a, const void *b)
{
	return strcmp(((const struct catalog_entry *)a)->ref, ((const struct catalog_entry *)b)->ref);
}

static void scan_root(struct catalog *c, const char *root, const char *source)
{
	DIR *d = opendir(root);
	if (!d)
		return;
	struct dirent *de;
	while ((de = readdir(d)) && c->n < CATALOG_MAX) {
		if (de->d_name[0] == '.')
			continue;
		char dir[4096];
		snprintf(dir, sizeof dir, "%s/%s", root, de->d_name);
		struct stat st;
		/* a widget folder is a real directory, not a link to one */
		if (lstat(dir, &st) || !S_ISDIR(st.st_mode))
			continue;
		struct catalog_entry *ne = realloc(c->e, (c->n + 1) * sizeof *ne);
		if (!ne)
			break;
		c->e = ne;
		struct catalog_entry *e = &c->e[c->n++];
		memset(e, 0, sizeof *e);
		e->dir = strdup(dir);
		snprintf(e->ref, sizeof e->ref, "%s/%.64s", source, de->d_name);
		if (!widget_id_valid(de->d_name))
			snprintf(e->err, sizeof e->err, "the folder name must be [a-z0-9][a-z0-9._-]*, at most 64 bytes");
		else
			e->ok = manifest_load(dir, &e->m, e->err, sizeof e->err);
	}
	closedir(d);
}

void catalog_scan(struct catalog *c, const char *builtin_root, const char *system_root, const char *user_root)
{
	memset(c, 0, sizeof *c);
	char ur[4096];
	if (!user_root && catalog_user_root(ur, sizeof ur))
		user_root = ur;
	const char *roots[3] = { builtin_root, system_root, user_root };
	static const char *const sources[3] = { "builtin", "system", "user" };
	for (int i = 0; i < 3; i++) {
		if (!roots[i])
			continue;
		c->roots[i] = strdup(roots[i]);
		scan_root(c, roots[i], sources[i]);
	}
	qsort(c->e, c->n, sizeof *c->e, by_ref);
}

void catalog_free(struct catalog *c)
{
	for (size_t i = 0; i < c->n; i++) {
		free(c->e[i].dir);
		if (c->e[i].ok)
			manifest_free(&c->e[i].m);
	}
	free(c->e);
	for (int i = 0; i < 3; i++)
		free(c->roots[i]);
	memset(c, 0, sizeof *c);
}

const struct catalog_entry *catalog_find(const struct catalog *c, const char *ref)
{
	for (size_t i = 0; i < c->n; i++)
		if (!strcmp(c->e[i].ref, ref))
			return &c->e[i];
	return NULL;
}
