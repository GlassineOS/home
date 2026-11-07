// SPDX-License-Identifier: GPL-2.0-or-later
#include "manifest.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pack.h"
#include "places.h"
#include "toml.h"

#define MAX_MANIFEST (64 * 1024)
#define MAX_PROPS 32
#define MAX_PROP_STR 256

static char *xstrdup(const char *s)
{
	char *p = strdup(s);
	if (!p)
		abort();
	return p;
}

static bool fail(char *err, size_t errlen, const char *fmt, const char *a)
{
	snprintf(err, errlen, fmt, a);
	return false;
}

long parse_duration(const char *s)
{
	long total = 0;
	if (!*s)
		return -1;
	while (*s) {
		if (*s < '0' || *s > '9')
			return -1;
		long n = 0;
		while (*s >= '0' && *s <= '9') {
			n = n * 10 + (*s++ - '0');
			if (n > 1000000)
				return -1;
		}
		long unit;
		switch (*s++) {
		case 'm': unit = 60; break;
		case 'h': unit = 3600; break;
		case 'd': unit = 86400; break;
		default: return -1;
		}
		total += n * unit;
		if (total > 366L * 86400)
			return -1;
	}
	return total;
}

bool widget_id_valid(const char *id)
{
	size_t n = strlen(id);
	if (n < 1 || n > 64)
		return false;
	if (!((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= '0' && id[0] <= '9')))
		return false;
	for (size_t i = 0; i < n; i++) {
		char c = id[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return false;
	}
	return true;
}

/* Paths that would let a widget read Home's own process state or devices, whatever the manifest says. */
bool path_forbidden_root(const char *p)
{
	static const char *const roots[] = { "/proc", "/sys", "/dev", "/run" };
	for (size_t i = 0; i < sizeof roots / sizeof *roots; i++) {
		size_t n = strlen(roots[i]);
		if (!strncmp(p, roots[i], n) && (p[n] == '/' || p[n] == 0))
			return true;
	}
	return false;
}

bool read_path_valid(const char *p, bool *dir)
{
	size_t n = strlen(p);
	if (n == 0 || n > MAX_PATH_LEN || !utf8_valid(p, n))
		return false;
	const char *c;
	if (p[0] == '/')
		c = p + 1;
	else if (p[0] == '~' && p[1] == '/')
		c = p + 2;
	else if (p[0] == '{') {
		const char *close = strchr(p, '}');
		if (!close || !place_known(p + 1, (size_t)(close - p - 1)) || close[1] != '/')
			return false;
		c = close + 2;
	} else
		return false;
	*dir = p[n - 1] == '/';
	if (!*c)
		return *dir && p[0] != '/'; /* "~/" and "{place}/" are allowed, "/" is not */
	const char *end = p + n - (*dir ? 1 : 0);
	while (c < end) {
		const char *s = memchr(c, '/', (size_t)(end - c));
		size_t len = s ? (size_t)(s - c) : (size_t)(end - c);
		if (len == 0 || (len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.'))
			return false;
		c += len + 1;
	}
	if (p[0] == '/' && path_forbidden_root(p))
		return false;
	return true;
}

static bool prop_key_valid(const char *k)
{
	size_t n = strlen(k);
	if (n < 1 || n > 32 || !((k[0] >= 'a' && k[0] <= 'z') || k[0] == '_'))
		return false;
	for (size_t i = 0; i < n; i++)
		if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= '0' && k[i] <= '9') || k[i] == '_'))
			return false;
	return true;
}

static bool icon_valid(const char *s)
{
	size_t n = strlen(s);
	if (n < 1 || n > 64)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-'))
			return false;
	return true;
}

static const struct value *want(const struct value *root, const char *key, enum vtype t, bool required,
				char *err, size_t errlen, bool *ok)
{
	static const char *const names[] = { "null", "a boolean", "an integer", "a number", "a string", "an array", "a table" };
	const struct value *v = v_map_get(root, key);
	if (!v) {
		if (required) {
			snprintf(err, errlen, "%s: missing", key);
			*ok = false;
		}
		return NULL;
	}
	if (v->t != t) {
		snprintf(err, errlen, "%s: must be %s", key, names[t]);
		*ok = false;
		return NULL;
	}
	return v;
}

bool manifest_parse(const char *text, size_t len, struct manifest *m, char *err, size_t errlen)
{
	memset(m, 0, sizeof *m);
	struct value *root = toml_parse(text, len, err, errlen);
	if (!root)
		return false;
	bool ok = true;
	static const char *const known[] = { "format", "name", "icon", "description", "sizes", "reads", "refresh", "props" };
	for (size_t i = 0; i < root->u.m.n && ok; i++) {
		bool k = false;
		for (size_t j = 0; j < sizeof known / sizeof *known; j++)
			if (!strcmp(root->u.m.k[i], known[j]))
				k = true;
		if (!k)
			ok = fail(err, errlen, "%s: unknown key", root->u.m.k[i]);
	}

	m->format = 1;
	const struct value *v;
	if (ok && (v = want(root, "format", V_INT, false, err, errlen, &ok))) {
		if (v->u.i < 1)
			ok = fail(err, errlen, "%s: must be 1 or more", "format");
		else if (v->u.i > WIDGET_FORMAT)
			ok = fail(err, errlen, "%s: this widget needs a newer Home", "format");
		else
			m->format = (int)v->u.i;
	}
	if (ok && (v = want(root, "name", V_STR, true, err, errlen, &ok))) {
		if (!v->u.s.len || v->u.s.len > 64 || strchr(v->u.s.p, '\n'))
			ok = fail(err, errlen, "%s: one line of 1 to 64 bytes", "name");
		else
			m->name = xstrdup(v->u.s.p);
	}
	if (ok && (v = want(root, "icon", V_STR, true, err, errlen, &ok))) {
		if (!icon_valid(v->u.s.p))
			ok = fail(err, errlen, "%s: an icon name of [a-z0-9_-]", "icon");
		else
			m->icon = xstrdup(v->u.s.p);
	}
	if (ok && (v = want(root, "description", V_STR, false, err, errlen, &ok))) {
		if (v->u.s.len > 200)
			ok = fail(err, errlen, "%s: at most 200 bytes", "description");
		else
			m->description = xstrdup(v->u.s.p);
	}
	if (ok && (v = want(root, "sizes", V_ARR, true, err, errlen, &ok))) {
		if (v->u.a.n < 1 || v->u.a.n > 2)
			ok = fail(err, errlen, "%s: one or two of \"half\", \"wide\"", "sizes");
		for (size_t i = 0; ok && i < v->u.a.n; i++) {
			const struct value *s = v->u.a.v[i];
			int size = 0;
			if (s->t == V_STR && !strcmp(s->u.s.p, "half"))
				size = SIZE_HALF;
			else if (s->t == V_STR && !strcmp(s->u.s.p, "wide"))
				size = SIZE_WIDE;
			if (!size || (i == 1 && m->sizes[0] == size))
				ok = fail(err, errlen, "%s: one or two of \"half\", \"wide\"", "sizes");
			else
				m->sizes[m->nsizes++] = size;
		}
	}
	if (ok && (v = want(root, "reads", V_ARR, false, err, errlen, &ok))) {
		if (v->u.a.n > MAX_READS)
			ok = fail(err, errlen, "%s: at most 16 entries", "reads");
		for (size_t i = 0; ok && i < v->u.a.n; i++) {
			const struct value *s = v->u.a.v[i];
			bool dir = false;
			const char *close = s->t == V_STR && s->u.s.p[0] == '{' ? strchr(s->u.s.p, '}') : NULL;
			if (close && !place_known(s->u.s.p + 1, (size_t)(close - s->u.s.p - 1)))
				ok = fail(err, errlen, "reads: \"%s\" names an unknown place (known: {books})", s->u.s.p);
			else if (s->t != V_STR || !read_path_valid(s->u.s.p, &dir))
				ok = fail(err, errlen, "reads: \"%s\" is not an allowed path (absolute, ~/ or {place}/, no . or .. "
						       "parts, not under /proc, /sys, /dev or /run)", s->t == V_STR ? s->u.s.p : "?");
			else {
				m->reads[m->nreads].path = xstrdup(s->u.s.p);
				m->reads[m->nreads++].dir = dir;
			}
		}
	}
	if (ok && (v = want(root, "refresh", V_MAP, false, err, errlen, &ok))) {
		for (size_t i = 0; ok && i < v->u.m.n; i++) {
			const char *k = v->u.m.k[i];
			const struct value *x = v->u.m.v[i];
			if (!strcmp(k, "on_change")) {
				if (x->t != V_BOOL)
					ok = fail(err, errlen, "%s: must be a boolean", "refresh.on_change");
				else
					m->on_change = x->u.b;
			} else if (!strcmp(k, "every")) {
				long s = x->t == V_STR ? parse_duration(x->u.s.p) : -1;
				if (s < 0)
					ok = fail(err, errlen, "%s: a duration like \"15m\", \"1h\" or \"1d\"", "refresh.every");
				else if (s < MIN_EVERY_S)
					ok = fail(err, errlen, "%s: at least 15 minutes", "refresh.every");
				else if (s % MIN_EVERY_S)
					ok = fail(err, errlen, "%s: a multiple of 15 minutes", "refresh.every");
				else
					m->every_s = s;
			} else {
				ok = fail(err, errlen, "refresh.%s: unknown key", k);
			}
		}
		if (ok && m->on_change && !m->nreads)
			ok = fail(err, errlen, "%s: on_change needs files in reads", "refresh");
	}
	m->props = v_map();
	if (ok && (v = want(root, "props", V_MAP, false, err, errlen, &ok))) {
		if (v->u.m.n > MAX_PROPS)
			ok = fail(err, errlen, "%s: at most 32 settings", "props");
		for (size_t i = 0; ok && i < v->u.m.n; i++) {
			const struct value *x = v->u.m.v[i];
			if (!prop_key_valid(v->u.m.k[i]))
				ok = fail(err, errlen, "props.%s: setting names are [a-z_][a-z0-9_]*, at most 32 bytes", v->u.m.k[i]);
			else if (x->t != V_STR && x->t != V_INT && x->t != V_FLOAT && x->t != V_BOOL)
				ok = fail(err, errlen, "props.%s: a default must be a string, number or boolean", v->u.m.k[i]);
			else if (x->t == V_STR && x->u.s.len > MAX_PROP_STR)
				ok = fail(err, errlen, "props.%s: at most 256 bytes", v->u.m.k[i]);
			else
				v_map_set(m->props, v->u.m.k[i], v_dup(x));
		}
	}
	v_free(root);
	if (!ok)
		manifest_free(m);
	return ok;
}

bool manifest_load(const char *dir, struct manifest *m, char *err, size_t errlen)
{
	char path[4096];
	memset(m, 0, sizeof *m);
	snprintf(path, sizeof path, "%s/widget.toml", dir);
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return fail(err, errlen, "widget.toml: %s", strerror(errno));
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > MAX_MANIFEST) {
		close(fd);
		return fail(err, errlen, "widget.toml: %s", "not a regular file of at most 64 KiB");
	}
	char *text = malloc((size_t)st.st_size + 1);
	ssize_t n = text ? read(fd, text, (size_t)st.st_size) : -1;
	close(fd);
	if (n != st.st_size) {
		free(text);
		return fail(err, errlen, "widget.toml: %s", "read failed");
	}
	bool ok = manifest_parse(text, (size_t)n, m, err, errlen);
	free(text);
	if (!ok)
		return false;
	snprintf(path, sizeof path, "%s/widget.lua", dir);
	if (access(path, R_OK)) {
		manifest_free(m);
		return fail(err, errlen, "widget.lua: %s", "missing");
	}
	snprintf(path, sizeof path, "%s/widget.edit.lua", dir);
	m->has_edit = !access(path, R_OK);
	return true;
}

void manifest_free(struct manifest *m)
{
	free(m->name);
	free(m->icon);
	free(m->description);
	for (size_t i = 0; i < m->nreads; i++)
		free(m->reads[i].path);
	v_free(m->props);
	memset(m, 0, sizeof *m);
}

static bool same_type(const struct value *def, const struct value *x)
{
	return def->t == x->t || (def->t == V_FLOAT && x->t == V_INT);
}

static struct value *coerce(const struct value *def, const struct value *x)
{
	if (def->t == V_FLOAT && x->t == V_INT)
		return v_float((double)x->u.i);
	return v_dup(x);
}

struct value *props_effective(const struct manifest *m, const struct value *saved)
{
	struct value *p = v_map();
	for (size_t i = 0; i < m->props->u.m.n; i++) {
		const struct value *def = m->props->u.m.v[i];
		const struct value *x = v_map_get(saved, m->props->u.m.k[i]);
		v_map_set(p, m->props->u.m.k[i], x && same_type(def, x) ? coerce(def, x) : v_dup(def));
	}
	return p;
}

bool props_apply(const struct manifest *m, struct value *props, const struct value *set, char *err, size_t errlen)
{
	if (!set || set->t != V_MAP)
		return fail(err, errlen, "%s", "ui.set needs a table of settings");
	for (size_t i = 0; i < set->u.m.n; i++) {
		const struct value *def = v_map_get(m->props, set->u.m.k[i]);
		if (!def)
			return fail(err, errlen, "ui.set: \"%s\" is not a setting of this widget", set->u.m.k[i]);
		if (!same_type(def, set->u.m.v[i]))
			return fail(err, errlen, "ui.set: \"%s\" has the wrong type", set->u.m.k[i]);
		if (set->u.m.v[i]->t == V_STR && set->u.m.v[i]->u.s.len > MAX_PROP_STR)
			return fail(err, errlen, "ui.set: \"%s\" is too long", set->u.m.k[i]);
	}
	for (size_t i = 0; i < set->u.m.n; i++)
		v_map_set(props, set->u.m.k[i], coerce(v_map_get(m->props, set->u.m.k[i]), set->u.m.v[i]));
	return true;
}
