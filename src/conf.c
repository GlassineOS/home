// SPDX-License-Identifier: GPL-2.0-or-later
#include "conf.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "manifest.h"
#include "pack.h"
#include "toml.h"

#define MAX_CONF (1024 * 1024)

bool conf_default_path(char *out, size_t len)
{
	const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	int n;
	if (x && x[0] == '/')
		n = snprintf(out, len, "%s/glassine/home.conf", x);
	else if (h && h[0] == '/')
		n = snprintf(out, len, "%s/.config/glassine/home.conf", h);
	else
		return false;
	return n > 0 && (size_t)n < len;
}

bool uuid_v4(char out[37])
{
	unsigned char b[16];
	if (getrandom(b, sizeof b, 0) != (ssize_t)sizeof b)
		return false;
	b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
	b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
	snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	return true;
}

static bool uuid_valid(const char *s)
{
	if (strlen(s) != 36)
		return false;
	for (int i = 0; i < 36; i++) {
		bool dash = i == 8 || i == 13 || i == 18 || i == 23;
		char c = s[i];
		if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}
	return true;
}

bool conf_widget_ref_valid(const char *ref)
{
	static const char *const src[] = { "builtin/", "system/", "user/", "home/" };
	for (size_t i = 0; i < sizeof src / sizeof *src; i++) {
		size_t n = strlen(src[i]);
		if (!strncmp(ref, src[i], n))
			return widget_id_valid(ref + n);
	}
	return false;
}

void conf_free(struct home_conf *c)
{
	for (size_t i = 0; i < c->n; i++) {
		free(c->w[i].widget);
		v_free(c->w[i].props);
	}
	free(c->w);
	for (size_t i = 0; i < c->nfav; i++)
		free(c->favourites[i]);
	free(c->favourites);
	memset(c, 0, sizeof *c);
}

static bool size_from(const struct value *v, int *size)
{
	if (v->t == V_STR && !strcmp(v->u.s.p, "half"))
		*size = SIZE_HALF;
	else if (v->t == V_STR && !strcmp(v->u.s.p, "wide"))
		*size = SIZE_WIDE;
	else if (v->t == V_INT && v->u.i > 0 && v->u.i < 6 && pack_size_valid((int)v->u.i))
		*size = (int)v->u.i;
	else
		return false;
	return true;
}

static bool props_ok(const struct value *p)
{
	if (p->t != V_MAP)
		return false;
	for (size_t i = 0; i < p->u.m.n; i++) {
		enum vtype t = p->u.m.v[i]->t;
		if (t != V_STR && t != V_INT && t != V_FLOAT && t != V_BOOL)
			return false;
	}
	return true;
}

static bool parse(const char *text, size_t len, struct home_conf *c, char *err, size_t errlen)
{
	struct value *root = toml_parse(text, len, err, errlen);
	if (!root)
		return false;
	bool ok = true;
	const struct value *ver = v_map_get(root, "version");
	if (!ver || ver->t != V_INT || ver->u.i < 1) {
		snprintf(err, errlen, "version: missing or not a version number");
		ok = false;
	} else if (ver->u.i > CONF_VERSION) {
		snprintf(err, errlen, "written by a newer Home (version %lld); not changing it", ver->u.i);
		c->read_only = true;
		ok = false;
	}
	const struct value *fav = ok ? v_map_get(root, "favourites") : NULL;
	if (fav) {
		if (fav->t != V_ARR) {
			snprintf(err, errlen, "favourites: must be a list of app ids");
			ok = false;
		}
		for (size_t i = 0; ok && i < fav->u.a.n; i++) {
			if (fav->u.a.v[i]->t != V_STR) {
				snprintf(err, errlen, "favourites: must be a list of app ids");
				ok = false;
				break;
			}
			char **nf = realloc(c->favourites, (c->nfav + 1) * sizeof *nf);
			if (!nf)
				abort();
			c->favourites = nf;
			c->favourites[c->nfav++] = strdup(fav->u.a.v[i]->u.s.p);
		}
	}
	const struct value *ws = ok ? v_map_get(root, "widget") : NULL;
	c->has_widgets = ws != NULL;
	if (ws && (ws->t != V_ARR || ws->u.a.n > CONF_MAX_WIDGETS)) {
		snprintf(err, errlen, "widget: must be [[widget]] tables, at most %d", CONF_MAX_WIDGETS);
		ok = false;
	}
	for (size_t i = 0; ok && ws && i < ws->u.a.n; i++) {
		const struct value *w = ws->u.a.v[i];
		const struct value *u = v_map_get(w, "uuid"), *id = v_map_get(w, "widget"), *sz = v_map_get(w, "size");
		const struct value *p = v_map_get(w, "props");
		int size;
		if (w->t != V_MAP || !u || u->t != V_STR || !uuid_valid(u->u.s.p) || conf_find(c, u->u.s.p) >= 0 ||
		    !id || id->t != V_STR || !conf_widget_ref_valid(id->u.s.p) || !sz || !size_from(sz, &size) ||
		    (p && !props_ok(p))) {
			snprintf(err, errlen, "widget %zu: needs a unique uuid, a widget (source/id), a size and flat props", i + 1);
			ok = false;
			break;
		}
		int k = conf_add(c, id->u.s.p, size, p);
		if (k < 0) {
			ok = false;
			break;
		}
		memcpy(c->w[k].uuid, u->u.s.p, 37);
	}
	v_free(root);
	return ok;
}

/* The file's bytes; NULL with *missing for no file, NULL with an error otherwise. */
static char *read_text(const char *path, size_t *len, bool *missing, char *err, size_t errlen)
{
	*missing = false;
	*len = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOENT)
			*missing = true;
		else
			snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return NULL;
	}
	struct stat st = { 0 };
	char *text = NULL;
	ssize_t n = -1;
	if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size <= MAX_CONF && (text = malloc((size_t)st.st_size + 1)))
		n = read(fd, text, (size_t)st.st_size);
	close(fd);
	if (n < 0 || n != st.st_size) {
		free(text);
		snprintf(err, errlen, "%s: can't read it", path);
		return NULL;
	}
	text[n] = 0;
	*len = (size_t)n;
	return text;
}

bool conf_parse(const char *text, size_t len, struct home_conf *c, char *err, size_t errlen)
{
	memset(c, 0, sizeof *c);
	err[0] = 0;
	if (parse(text, len, c, err, errlen))
		return true;
	bool ro = c->read_only;
	conf_free(c);
	c->read_only = ro;
	return false;
}

char *conf_read(const char *path, size_t *len, enum conf_state *state, struct home_conf *c, char *err, size_t errlen)
{
	memset(c, 0, sizeof *c);
	err[0] = 0;
	bool missing;
	char *text = read_text(path, len, &missing, err, errlen);
	if (!text) {
		*state = missing ? CONF_MISSING : CONF_UNREADABLE;
		return NULL;
	}
	if (parse(text, *len, c, err, errlen)) {
		*state = CONF_OK;
		return text;
	}
	*state = c->read_only ? CONF_NEWER : CONF_BROKEN;
	conf_free(c);
	return text;
}

bool conf_load(const char *path, struct home_conf *c, char *err, size_t errlen)
{
	size_t len;
	enum conf_state st;
	free(conf_read(path, &len, &st, c, err, errlen));
	if (st == CONF_OK || st == CONF_MISSING)
		return true;
	/* broken, newer or unreadable: the file stays where it is, untouched and not to be overwritten */
	c->read_only = true;
	return false;
}

char *conf_format(const struct home_conf *c, size_t *len, char *err, size_t errlen)
{
	struct buf b = { 0 };
	buf_adds(&b, "# glassine-home: the widgets in order. Pages follow from packing.\n");
	buf_addf(&b, "version = %d\n", CONF_VERSION);
	if (c->nfav) {
		buf_adds(&b, "favourites = [");
		for (size_t i = 0; i < c->nfav; i++) {
			struct value *s = v_str(c->favourites[i]);
			if (i)
				buf_adds(&b, ", ");
			toml_put_value(&b, s);
			v_free(s);
		}
		buf_adds(&b, "]\n");
	}
	if (!c->n)
		buf_adds(&b, "widget = []\n"); /* emptied on purpose; no widget key = never laid out */
	for (size_t i = 0; i < c->n; i++) {
		const struct instance *w = &c->w[i];
		struct value *id = v_str(w->widget);
		buf_addf(&b, "\n[[widget]]\nuuid = \"%s\"\nwidget = ", w->uuid);
		toml_put_value(&b, id);
		v_free(id);
		if (w->size == SIZE_HALF)
			buf_adds(&b, "\nsize = \"half\"\n");
		else if (w->size == SIZE_WIDE)
			buf_adds(&b, "\nsize = \"wide\"\n");
		else
			buf_addf(&b, "\nsize = %d\n", w->size);
		if (w->props && w->props->u.m.n) {
			buf_adds(&b, "props = ");
			if (!toml_put_value(&b, w->props)) {
				snprintf(err, errlen, "widget %zu: settings can't be written", i + 1);
				buf_free(&b);
				return NULL;
			}
			buf_adds(&b, "\n");
		}
	}
	buf_add(&b, "", 1);
	*len = b.len - 1;
	return b.p;
}

/* the config directories, if new (~/.config, then glassine) */
static void make_dirs(const char *path)
{
	char dirbuf[4096], parent[4096];
	snprintf(dirbuf, sizeof dirbuf, "%s", path);
	char *dir = dirname(dirbuf);
	snprintf(parent, sizeof parent, "%s", dir);
	mkdir(dirname(parent), 0700);
	mkdir(dir, 0700);
}

int conf_lock(const char *path, int timeout_ms)
{
	char lp[4200];
	snprintf(lp, sizeof lp, "%s.lock", path);
	make_dirs(path);
	int fd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -1;
	if (timeout_ms < 0) {
		while (flock(fd, LOCK_EX) < 0)
			if (errno != EINTR) {
				close(fd);
				return -1;
			}
		return fd;
	}
	struct timespec t0, now;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		if (!flock(fd, LOCK_EX | LOCK_NB))
			return fd;
		if (errno != EWOULDBLOCK && errno != EINTR)
			break;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L >= timeout_ms)
			break;
		struct timespec ts = { 0, 5 * 1000000L };
		nanosleep(&ts, NULL);
	}
	close(fd);
	errno = EWOULDBLOCK;
	return -1;
}

void conf_unlock(int fd)
{
	if (fd >= 0)
		close(fd); /* closing the last descriptor releases the lock */
}

bool conf_write_text(const char *path, const char *text, size_t len, char *err, size_t errlen)
{
	char dirbuf[4096], tmp[4200];
	snprintf(dirbuf, sizeof dirbuf, "%s", path);
	char *dir = dirname(dirbuf);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	make_dirs(path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	bool ok = fd >= 0;
	size_t off = 0;
	while (ok && off < len) {
		ssize_t w = write(fd, text + off, len - off);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			ok = false;
		else
			off += (size_t)w;
	}
	if (ok && fsync(fd))
		ok = false;
	if (fd >= 0 && close(fd))
		ok = false;
	if (ok && rename(tmp, path))
		ok = false;
	if (!ok) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		unlink(tmp);
	} else {
		int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (dfd >= 0) {
			fsync(dfd);
			close(dfd);
		}
	}
	return ok;
}

bool conf_save(const char *path, const struct home_conf *c, char *err, size_t errlen)
{
	if (c->read_only) {
		snprintf(err, errlen, "the layout was written by a newer Home; not overwriting it");
		return false;
	}
	size_t len;
	char *text = conf_format(c, &len, err, errlen);
	if (!text)
		return false;
	int lk = conf_lock(path, -1);
	if (lk < 0) {
		snprintf(err, errlen, "%s.lock: %s", path, strerror(errno));
		free(text);
		return false;
	}
	bool ok = conf_write_text(path, text, len, err, errlen);
	conf_unlock(lk);
	free(text);
	return ok;
}

bool conf_favourites_equal(const struct home_conf *a, const struct home_conf *b)
{
	if (a->nfav != b->nfav)
		return false;
	for (size_t i = 0; i < a->nfav; i++)
		if (strcmp(a->favourites[i], b->favourites[i]))
			return false;
	return true;
}

static bool props_equal(const struct value *a, const struct value *b)
{
	size_t na = a ? a->u.m.n : 0, nb = b ? b->u.m.n : 0;
	if (!na || !nb)
		return na == nb;
	return v_equal(a, b);
}

bool conf_instance_equal(const struct instance *a, const struct instance *b)
{
	return !strcmp(a->uuid, b->uuid) && !strcmp(a->widget, b->widget) && a->size == b->size &&
	       props_equal(a->props, b->props);
}

bool conf_widgets_equal(const struct home_conf *a, const struct home_conf *b)
{
	if (a->n != b->n)
		return false;
	for (size_t i = 0; i < a->n; i++)
		if (!conf_instance_equal(&a->w[i], &b->w[i]))
			return false;
	return true;
}

void conf_take_favourites(struct home_conf *dst, struct home_conf *src)
{
	for (size_t i = 0; i < dst->nfav; i++)
		free(dst->favourites[i]);
	free(dst->favourites);
	dst->favourites = src->favourites;
	dst->nfav = src->nfav;
	src->favourites = NULL;
	src->nfav = 0;
}

void conf_take_widgets(struct home_conf *dst, struct home_conf *src)
{
	for (size_t i = 0; i < dst->n; i++) {
		free(dst->w[i].widget);
		v_free(dst->w[i].props);
	}
	free(dst->w);
	dst->w = src->w;
	dst->n = src->n;
	src->w = NULL;
	src->n = 0;
}

int conf_find(const struct home_conf *c, const char *uuid)
{
	for (size_t i = 0; i < c->n; i++)
		if (!strcmp(c->w[i].uuid, uuid))
			return (int)i;
	return -1;
}

int conf_add(struct home_conf *c, const char *widget, int size, const struct value *props)
{
	if (c->n >= CONF_MAX_WIDGETS || !conf_widget_ref_valid(widget) || !pack_size_valid(size))
		return -1;
	struct instance *nw = realloc(c->w, (c->n + 1) * sizeof *nw);
	if (!nw)
		return -1;
	c->w = nw;
	struct instance *w = &c->w[c->n];
	memset(w, 0, sizeof *w);
	if (!uuid_v4(w->uuid))
		return -1;
	w->widget = strdup(widget);
	w->size = size;
	w->props = props ? v_dup(props) : v_map();
	return (int)c->n++;
}

bool conf_remove(struct home_conf *c, size_t i)
{
	if (i >= c->n)
		return false;
	free(c->w[i].widget);
	v_free(c->w[i].props);
	memmove(&c->w[i], &c->w[i + 1], (c->n - i - 1) * sizeof *c->w);
	c->n--;
	return true;
}

bool conf_move(struct home_conf *c, size_t from, size_t to)
{
	if (from >= c->n || to >= c->n)
		return false;
	struct instance t = c->w[from];
	if (from < to)
		memmove(&c->w[from], &c->w[from + 1], (to - from) * sizeof *c->w);
	else
		memmove(&c->w[to + 1], &c->w[to], (from - to) * sizeof *c->w);
	c->w[to] = t;
	return true;
}

bool conf_reorder(struct home_conf *c, const size_t *order)
{
	struct instance *nw = malloc((c->n ? c->n : 1) * sizeof *nw);
	if (!nw)
		return false;
	for (size_t i = 0; i < c->n; i++) {
		if (order[i] >= c->n) {
			free(nw);
			return false;
		}
		nw[i] = c->w[order[i]];
	}
	free(c->w);
	c->w = nw;
	return true;
}
