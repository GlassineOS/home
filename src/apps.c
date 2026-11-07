// SPDX-License-Identifier: GPL-2.0-or-later
#include "apps.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const CATS[] = { "Education", "Games", "Internet", "Multimedia", "Office", "Reading", "System", NULL };

const char *const *apps_categories(void)
{
	return CATS;
}

/* the shell drawer's map (GlassineOS/shell glassine-shell.c category()) */
const char *apps_category_of(const char *cats)
{
	static const struct { const char *fd, *cat; } map[] = {
		{ "Viewer", "Reading" }, { "Literature", "Reading" }, { "Dictionary", "Reading" },
		{ "Presentation", "Office" }, { "Office", "Office" }, { "Education", "Education" },
		{ "Science", "Education" }, { "Game", "Games" }, { "AudioVideo", "Multimedia" },
		{ "Audio", "Multimedia" }, { "Video", "Multimedia" }, { "Graphics", "Office" },
		{ "Network", "Internet" }, { "WebBrowser", "Internet" }, { "Settings", "System" },
		{ "System", "System" }, { "Development", "System" }, { "Utility", "System" },
	};
	for (size_t i = 0; i < sizeof map / sizeof *map; i++) {
		size_t l = strlen(map[i].fd);
		for (const char *p = cats; (p = strstr(p, map[i].fd)); p += l)
			if ((p == cats || p[-1] == ';') && (p[l] == ';' || !p[l]))
				return map[i].cat;
	}
	return "System";
}

size_t apps_dirs(char dirs[][1024], size_t max)
{
	size_t n = 0;
	const char *home = getenv("HOME") ? getenv("HOME") : "";
	const char *dh = getenv("XDG_DATA_HOME");
	if (max == 0)
		return 0;
	/* tests: the application directories themselves (XDG_DATA_DIRS also finds image loaders) */
	const char *only = getenv("GLASSINE_HOME_APPS");
	if (only && *only) {
		while (*only && n < max) {
			const char *e = strchr(only, ':');
			int len = e ? (int)(e - only) : (int)strlen(only);
			if (len > 0)
				snprintf(dirs[n++], 1024, "%.*s", len, only);
			only += len + (e ? 1 : 0);
		}
		return n;
	}
	if (dh && *dh)
		snprintf(dirs[n++], 1024, "%s/applications", dh);
	else
		snprintf(dirs[n++], 1024, "%s/.local/share/applications", home);
	const char *dd = getenv("XDG_DATA_DIRS");
	if (!dd || !*dd)
		dd = "/usr/local/share:/usr/share";
	while (*dd && n < max) {
		const char *e = strchr(dd, ':');
		int len = e ? (int)(e - dd) : (int)strlen(dd);
		if (len > 0)
			snprintf(dirs[n++], 1024, "%.*s/applications", len, dd);
		dd += len + (e ? 1 : 0);
	}
	return n;
}

/* a desktop file's OnlyShowIn/NotShowIn list names one of $XDG_CURRENT_DESKTOP's */
static bool desktop_listed(const char *list)
{
	const char *cur = getenv("XDG_CURRENT_DESKTOP");
	char d[128];
	snprintf(d, sizeof d, "%s", cur && *cur ? cur : "sway");
	char *sv = NULL;
	for (char *t = strtok_r(d, ":", &sv); t; t = strtok_r(NULL, ":", &sv)) {
		size_t n = strlen(t);
		for (const char *q = list; *q;) {
			size_t m = strcspn(q, ";");
			if (m == n && !strncasecmp(q, t, n))
				return true;
			q += m + (q[m] == ';');
		}
	}
	return false;
}

static void load_one(struct apps *l, const char *dir, const char *fn)
{
	char p[2200];
	snprintf(p, sizeof p, "%s/%s", dir, fn);
	FILE *f = fopen(p, "re");
	if (!f)
		return;
	struct app a = { 0 };
	snprintf(a.id, sizeof a.id, "%.*s", (int)(strlen(fn) - 8), fn);
	char line[1024], cats[256] = "", exec[8] = "";
	bool in = false, hide = false, isapp = false;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			in = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		char *v = in ? strchr(line, '=') : NULL;
		if (!v)
			continue;
		*v++ = 0;
		if (!strcmp(line, "Name"))
			snprintf(a.name, sizeof a.name, "%s", v);
		else if (!strcmp(line, "Icon"))
			snprintf(a.icon, sizeof a.icon, "%s", v);
		else if (!strcmp(line, "Exec"))
			snprintf(exec, sizeof exec, "%s", v);
		else if (!strcmp(line, "Categories"))
			snprintf(cats, sizeof cats, "%s", v);
		else if (!strcmp(line, "NoDisplay") || !strcmp(line, "Hidden"))
			hide |= !strcmp(v, "true");
		else if (!strcmp(line, "Type"))
			isapp = !strcmp(v, "Application");
		else if (!strcmp(line, "OnlyShowIn"))
			hide |= !desktop_listed(v);
		else if (!strcmp(line, "NotShowIn"))
			hide |= desktop_listed(v);
	}
	fclose(f);
	if (!isapp || hide || !a.name[0] || !exec[0])
		return;
	for (size_t i = 0; i < l->n; i++)
		if (!strcmp(l->a[i].id, a.id))
			return; /* the first directory wins */
	a.cat = apps_category_of(cats);
	if (strstr(dir, "/flatpak/exports/"))
		snprintf(a.origin, sizeof a.origin, "Flatpak");
	struct app *na = realloc(l->a, (l->n + 1) * sizeof *na);
	if (!na)
		return;
	l->a = na;
	l->a[l->n++] = a;
}

static int by_name(const void *x, const void *y)
{
	const struct app *a = x, *b = y;
	int c = strcasecmp(a->name, b->name);
	return c ? c : strcmp(a->id, b->id);
}

void apps_load(struct apps *l)
{
	apps_free(l);
	char dirs[16][1024];
	size_t nd = apps_dirs(dirs, 16);
	for (size_t d = 0; d < nd; d++) {
		DIR *dd = opendir(dirs[d]);
		if (!dd)
			continue;
		struct dirent *e;
		while ((e = readdir(dd))) {
			size_t len = strlen(e->d_name);
			if (len > 8 && !strcmp(e->d_name + len - 8, ".desktop"))
				load_one(l, dirs[d], e->d_name);
		}
		closedir(dd);
	}
	if (l->n)
		qsort(l->a, l->n, sizeof *l->a, by_name);
	for (size_t i = 0; i + 1 < l->n; i++)
		if (!strcasecmp(l->a[i].name, l->a[i + 1].name))
			l->a[i].twin = l->a[i + 1].twin = true;
	apps_load_opened(l);
}

void apps_free(struct apps *l)
{
	free(l->a);
	l->a = NULL;
	l->n = 0;
}

const struct app *apps_find(const struct apps *l, const char *id)
{
	for (size_t i = 0; i < l->n; i++)
		if (!strcmp(l->a[i].id, id))
			return &l->a[i];
	return NULL;
}

const struct app *apps_resolve(const struct apps *l, const char *id)
{
	const struct app *exact = apps_find(l, id), *best = NULL;
	if (exact)
		return exact;
	for (size_t i = 0; i < l->n; i++) {
		const char *dot = strrchr(l->a[i].id, '.');
		if (dot && !strcasecmp(dot + 1, id) && (!best || l->a[i].opened > best->opened))
			best = &l->a[i];
	}
	return best;
}

size_t apps_count_in(const struct apps *l, const char *cat)
{
	size_t n = 0;
	for (size_t i = 0; i < l->n; i++)
		n += !strcmp(l->a[i].cat, cat);
	return n;
}

struct value *apps_ctx(const struct apps *l)
{
	struct value *root = v_map(), *cats = v_arr();
	for (const char *const *c = CATS; *c; c++) {
		size_t n = apps_count_in(l, *c);
		if (!n)
			continue;
		struct value *m = v_map();
		v_map_set(m, "name", v_str(*c));
		v_map_set(m, "count", v_int((long long)n));
		v_arr_push(cats, m);
	}
	v_map_set(root, "categories", cats);
	return root;
}

static bool opened_path(char *out, size_t len, bool mk)
{
	const char *x = getenv("XDG_STATE_HOME"), *h = getenv("HOME");
	char dir[1024];
	if (x && x[0] == '/')
		snprintf(dir, sizeof dir, "%s/glassine-home", x);
	else if (h && h[0] == '/')
		snprintf(dir, sizeof dir, "%s/.local/state/glassine-home", h);
	else
		return false;
	if (mk) {
		char p[1024];
		snprintf(p, sizeof p, "%s", dir);
		for (char *s = p + 1; *s; s++)
			if (*s == '/') {
				*s = 0;
				mkdir(p, 0755);
				*s = '/';
			}
		mkdir(p, 0755);
	}
	return snprintf(out, len, "%s/opened", dir) < (int)len;
}

void apps_load_opened(struct apps *l)
{
	char p[1100];
	if (!opened_path(p, sizeof p, false))
		return;
	FILE *f = fopen(p, "re");
	if (!f)
		return;
	char line[APP_ID_MAX + 32];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		char *sp = strchr(line, ' ');
		if (!sp)
			continue;
		*sp = 0;
		for (size_t i = 0; i < l->n; i++)
			if (!strcmp(l->a[i].id, sp + 1))
				l->a[i].opened = atoll(line);
	}
	fclose(f);
}

void apps_mark_opened(struct apps *l, const char *id, long long now)
{
	for (size_t i = 0; i < l->n; i++)
		if (!strcmp(l->a[i].id, id))
			l->a[i].opened = now;
	char p[1100], tmp[1200];
	if (!opened_path(p, sizeof p, true))
		return;
	snprintf(tmp, sizeof tmp, "%s.tmp", p);
	FILE *f = fopen(tmp, "we");
	if (!f)
		return;
	for (size_t i = 0; i < l->n; i++)
		if (l->a[i].opened)
			fprintf(f, "%lld %s\n", l->a[i].opened, l->a[i].id);
	if (fclose(f) == 0)
		rename(tmp, p);
	else
		unlink(tmp);
}
