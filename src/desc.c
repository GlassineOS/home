// SPDX-License-Identifier: GPL-2.0-or-later
#include "desc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

struct nctx {
	bool edit;
	desc_path_check check;
	void *ctx;
	char *err;
	size_t errlen;
};

static bool nfail(struct nctx *c, const char *fmt, const char *a)
{
	snprintf(c->err, c->errlen, fmt, a);
	return false;
}

/* A one-line string: valid UTF-8, control characters become spaces, cut at max bytes. */
static struct value *line(struct nctx *c, const struct value *v, size_t max, const char *what)
{
	if (!v || v->t != V_STR) {
		nfail(c, "%s must be a string", what);
		return NULL;
	}
	if (!utf8_valid(v->u.s.p, v->u.s.len)) {
		nfail(c, "%s is not valid UTF-8", what);
		return NULL;
	}
	size_t n = utf8_cut(v->u.s.p, v->u.s.len, max);
	struct value *r = v_strn(v->u.s.p, n);
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)r->u.s.p[i] < 0x20 || r->u.s.p[i] == 0x7f)
			r->u.s.p[i] = ' ';
	return r;
}

static bool opt_line(struct nctx *c, struct value *out, const struct value *in, const char *key, size_t max, const char *what)
{
	const struct value *v = v_map_get(in, key);
	if (!v || v->t == V_NULL)
		return true;
	struct value *s = line(c, v, max, what);
	if (!s)
		return false;
	v_map_set(out, key, s);
	return true;
}

static bool req_line(struct nctx *c, struct value *out, const struct value *in, const char *key, size_t max, const char *what)
{
	struct value *s = line(c, v_map_get(in, key), max, what);
	if (!s)
		return false;
	v_map_set(out, key, s);
	return true;
}

static bool is_true(const struct value *v) { return v && v->t == V_BOOL && v->u.b; }

static bool only_keys(struct nctx *c, const struct value *in, const char *const *keys, const char *what)
{
	for (size_t i = 0; i < in->u.m.n; i++) {
		bool k = false;
		for (const char *const *q = keys; *q; q++)
			if (!strcmp(in->u.m.k[i], *q))
				k = true;
		if (!k) {
			char msg[128];
			snprintf(msg, sizeof msg, "%s: unknown field \"%%s\"", what);
			return nfail(c, msg, in->u.m.k[i]);
		}
	}
	return true;
}

static bool app_id_valid(const char *s, size_t n)
{
	if (n < 1 || n > 128)
		return false;
	for (size_t i = 0; i < n; i++) {
		char ch = s[i];
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-'))
			return false;
	}
	return true;
}

static bool abs_path_valid(const char *s, size_t n)
{
	return n >= 2 && n <= 4096 && s[0] == '/' && strlen(s) == n && utf8_valid(s, n);
}

static bool page_valid(const char *s, size_t n)
{
	if (n < 1 || n > 128)
		return false;
	for (size_t i = 0; i < n; i++) {
		char ch = s[i];
		if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '/' || ch == '.' || ch == '_' || ch == '-'))
			return false;
	}
	return true;
}

static struct value *action(struct nctx *c, const struct value *in)
{
	if (!in || in->t != V_MAP) {
		nfail(c, "%s", "an action must come from ui.open, ui.settings or ui.set");
		return NULL;
	}
	const struct value *a = v_map_get(in, "a");
	const char *kind = a && a->t == V_STR ? a->u.s.p : "";
	struct value *out = v_map();
	if (!strcmp(kind, "open")) {
		static const char *const keys[] = { "a", "app", "file", NULL };
		const struct value *app = v_map_get(in, "app"), *file = v_map_get(in, "file");
		if (!only_keys(c, in, keys, "ui.open"))
			goto bad;
		if (!app || app->t != V_STR || !app_id_valid(app->u.s.p, app->u.s.len)) {
			nfail(c, "%s", "ui.open: the app must be an application id");
			goto bad;
		}
		v_map_set(out, "a", v_str("open"));
		v_map_set(out, "app", v_dup(app));
		if (file && file->t != V_NULL) {
			if (file->t != V_STR || !abs_path_valid(file->u.s.p, file->u.s.len)) {
				nfail(c, "%s", "ui.open: the file must be an absolute path");
				goto bad;
			}
			v_map_set(out, "file", v_dup(file));
		}
		return out;
	}
	if (!strcmp(kind, "settings")) {
		static const char *const keys[] = { "a", "page", NULL };
		const struct value *page = v_map_get(in, "page");
		if (!only_keys(c, in, keys, "ui.settings"))
			goto bad;
		if (!page || page->t != V_STR || !page_valid(page->u.s.p, page->u.s.len)) {
			nfail(c, "%s", "ui.settings: the page must be a Settings page name");
			goto bad;
		}
		v_map_set(out, "a", v_str("settings"));
		v_map_set(out, "page", v_dup(page));
		return out;
	}
	if (!strcmp(kind, "set")) {
		static const char *const keys[] = { "a", "props", NULL };
		const struct value *p = v_map_get(in, "props");
		if (!c->edit) {
			nfail(c, "%s", "ui.set works only in widget.edit.lua");
			goto bad;
		}
		if (!only_keys(c, in, keys, "ui.set"))
			goto bad;
		if (!p || p->t != V_MAP || p->u.m.n == 0 || p->u.m.n > 32) {
			nfail(c, "%s", "ui.set needs a table of 1 to 32 settings");
			goto bad;
		}
		for (size_t i = 0; i < p->u.m.n; i++) {
			const struct value *x = p->u.m.v[i];
			if (x->t != V_STR && x->t != V_INT && x->t != V_FLOAT && x->t != V_BOOL) {
				nfail(c, "ui.set: \"%s\" must be a string, number or boolean", p->u.m.k[i]);
				goto bad;
			}
			if (x->t == V_STR && (x->u.s.len > 256 || !utf8_valid(x->u.s.p, x->u.s.len))) {
				nfail(c, "ui.set: \"%s\" must be UTF-8 of at most 256 bytes", p->u.m.k[i]);
				goto bad;
			}
			if (x->t == V_FLOAT && !isfinite(x->u.f)) {
				nfail(c, "ui.set: \"%s\" must be a finite number", p->u.m.k[i]);
				goto bad;
			}
		}
		v_map_set(out, "a", v_str("set"));
		v_map_set(out, "props", v_dup(p));
		return out;
	}
	nfail(c, "%s", "an action must come from ui.open, ui.settings or ui.set");
bad:
	v_free(out);
	return NULL;
}

/* A pixel size: a whole number from 1 to IMAGE_MAX_PX. */
static bool px(const struct value *v, long long *out)
{
	double d;
	if (!v)
		return false;
	if (v->t == V_INT)
		d = (double)v->u.i;
	else if (v->t == V_FLOAT)
		d = v->u.f;
	else
		return false;
	if (!(d >= 1 && d <= IMAGE_MAX_PX) || d != floor(d))
		return false;
	*out = (long long)d;
	return true;
}

static struct value *image(struct nctx *c, const struct value *in)
{
	static const char *const keys[] = { "t", "src", "w", "h", "side", "fit", "alt", NULL };
	if (!only_keys(c, in, keys, "ui.image"))
		return NULL;
	struct value *out = v_map();
	v_map_set(out, "t", v_str("image"));
	const struct value *src = v_map_get(in, "src");
	if (!src || src->t != V_STR || src->u.s.len > 4200) {
		nfail(c, "%s", "ui.image: src must be file:PATH, cover:PATH or icon:APP");
		goto bad;
	}
	const char *s = src->u.s.p;
	size_t n = src->u.s.len;
	if (!strncmp(s, "file:", 5)) {
		if (!abs_path_valid(s + 5, n - 5)) {
			nfail(c, "%s", "ui.image: file: needs an absolute path");
			goto bad;
		}
		if (c->check && !c->check(c->ctx, s + 5)) {
			nfail(c, "ui.image: %s is not in the widget's reads", s + 5);
			goto bad;
		}
	} else if (!strncmp(s, "cover:", 6)) {
		if (!abs_path_valid(s + 6, n - 6)) {
			nfail(c, "%s", "ui.image: cover: needs an absolute path");
			goto bad;
		}
	} else if (!strncmp(s, "icon:", 5)) {
		if (!app_id_valid(s + 5, n - 5)) {
			nfail(c, "%s", "ui.image: icon: needs an application id");
			goto bad;
		}
	} else {
		nfail(c, "%s", "ui.image: src must be file:PATH, cover:PATH or icon:APP (no network sources)");
		goto bad;
	}
	v_map_set(out, "src", v_dup(src));

	long long w = 0, h = 0;
	const struct value *side = v_map_get(in, "side"), *fit = v_map_get(in, "fit"), *wv = v_map_get(in, "w");
	const char *sd = side && side->t == V_STR ? side->u.s.p : NULL;
	if (side && side->t != V_NULL && (!sd || (strcmp(sd, "left") && strcmp(sd, "right") && strcmp(sd, "none")))) {
		nfail(c, "%s", "ui.image: side must be \"left\", \"right\" or \"none\"");
		goto bad;
	}
	if (sd && !strcmp(sd, "none"))
		sd = NULL;
	if (!px(v_map_get(in, "h"), &h)) {
		nfail(c, "%s", "ui.image: h must be a whole number of pixels from 1 to 2000");
		goto bad;
	}
	if (wv && wv->t != V_NULL && !px(wv, &w)) {
		nfail(c, "%s", "ui.image: w must be a whole number of pixels from 1 to 2000");
		goto bad;
	}
	if (sd && !w) {
		nfail(c, "%s", "ui.image: an image beside the text needs w");
		goto bad;
	}
	v_map_set(out, "h", v_int(h));
	if (w)
		v_map_set(out, "w", v_int(w));
	if (sd)
		v_map_set(out, "side", v_str(sd));
	const char *ft = "cover";
	if (fit && fit->t != V_NULL) {
		if (fit->t != V_STR || (strcmp(fit->u.s.p, "cover") && strcmp(fit->u.s.p, "contain"))) {
			nfail(c, "%s", "ui.image: fit must be \"cover\" or \"contain\"");
			goto bad;
		}
		ft = fit->u.s.p;
	}
	v_map_set(out, "fit", v_str(ft));
	if (!req_line(c, out, in, "alt", 200, "ui.image: alt"))
		goto bad;
	return out;
bad:
	v_free(out);
	return NULL;
}

static struct value *list(struct nctx *c, const struct value *in)
{
	static const char *const keys[] = { "t", "rows", NULL };
	static const char *const rkeys[] = { "label", "meta", "selected", "action", NULL };
	if (!only_keys(c, in, keys, "ui.list"))
		return NULL;
	const struct value *rows = v_map_get(in, "rows");
	if (!rows || rows->t != V_ARR) {
		nfail(c, "%s", "ui.list needs a list of rows");
		return NULL;
	}
	struct value *out = v_map(), *ro = v_arr();
	v_map_set(out, "t", v_str("list"));
	v_map_set(out, "rows", ro);
	/* rows past the fourth are cut, not checked */
	for (size_t i = 0; i < rows->u.a.n && i < DESC_MAX_ROWS; i++) {
		const struct value *r = rows->u.a.v[i];
		if (r->t != V_MAP) {
			nfail(c, "%s", "ui.list: each row is { label, meta }");
			goto bad;
		}
		if (!only_keys(c, r, rkeys, "ui.list row"))
			goto bad;
		struct value *o = v_map();
		v_arr_push(ro, o);
		if (!req_line(c, o, r, "label", 120, "ui.list: a row's label") ||
		    !opt_line(c, o, r, "meta", 60, "ui.list: a row's meta"))
			goto bad;
		const struct value *sel = v_map_get(r, "selected"), *act = v_map_get(r, "action");
		if (sel && sel->t != V_NULL) {
			if (!c->edit) {
				nfail(c, "%s", "selected works only in widget.edit.lua");
				goto bad;
			}
			if (is_true(sel))
				v_map_set(o, "selected", v_bool(true));
		}
		if (act && act->t != V_NULL) {
			struct value *a = action(c, act);
			if (!a)
				goto bad;
			v_map_set(o, "action", a);
		}
	}
	return out;
bad:
	v_free(out);
	return NULL;
}

struct value *desc_normalize(const struct value *in, bool edit, desc_path_check check, void *ctx,
			     char *err, size_t errlen)
{
	struct nctx c = { edit, check, ctx, err, errlen };
	if (!in || in->t != V_ARR) {
		nfail(&c, "%s", "render must return a list of ui primitives");
		return NULL;
	}
	if (in->u.a.n > DESC_MAX_ITEMS) {
		nfail(&c, "%s", "render returned more than 12 primitives");
		return NULL;
	}
	struct value *out = v_arr();
	int titles = 0, buttons = 0, images = 0;
	for (size_t i = 0; i < in->u.a.n; i++) {
		const struct value *p = in->u.a.v[i];
		const struct value *t = p->t == V_MAP ? v_map_get(p, "t") : NULL;
		const char *kind = t && t->t == V_STR ? t->u.s.p : "";
		struct value *o = NULL;
		if (!strcmp(kind, "title")) {
			static const char *const keys[] = { "t", "text", NULL };
			if (++titles > 1) {
				nfail(&c, "%s", "at most one ui.title");
				goto bad;
			}
			if (!only_keys(&c, p, keys, "ui.title"))
				goto bad;
			o = v_map();
			v_map_set(o, "t", v_str("title"));
			if (!req_line(&c, o, p, "text", 120, "ui.title: the text"))
				goto bad_o;
		} else if (!strcmp(kind, "text")) {
			static const char *const keys[] = { "t", "text", "soft", NULL };
			if (!only_keys(&c, p, keys, "ui.text"))
				goto bad;
			o = v_map();
			v_map_set(o, "t", v_str("text"));
			if (!req_line(&c, o, p, "text", 200, "ui.text: the text"))
				goto bad_o;
			if (is_true(v_map_get(p, "soft")))
				v_map_set(o, "soft", v_bool(true));
		} else if (!strcmp(kind, "meta")) {
			static const char *const keys[] = { "t", "text", "time", NULL };
			if (!only_keys(&c, p, keys, "ui.meta"))
				goto bad;
			o = v_map();
			v_map_set(o, "t", v_str("meta"));
			if (!req_line(&c, o, p, "text", 120, "ui.meta: the text"))
				goto bad_o;
			const struct value *tm = v_map_get(p, "time");
			if (tm && tm->t != V_NULL) {
				double d = tm->t == V_INT ? (double)tm->u.i : tm->t == V_FLOAT ? tm->u.f : NAN;
				if (!(d >= 0 && d < 1e11)) {
					nfail(&c, "%s", "ui.meta: time must be seconds since 1970");
					goto bad_o;
				}
				v_map_set(o, "time", v_int((long long)floor(d)));
			}
		} else if (!strcmp(kind, "progress")) {
			static const char *const keys[] = { "t", "value", "label", NULL };
			if (!only_keys(&c, p, keys, "ui.progress"))
				goto bad;
			const struct value *v = v_map_get(p, "value");
			double d = v && v->t == V_INT ? (double)v->u.i : v && v->t == V_FLOAT ? v->u.f : NAN;
			if (isnan(d)) {
				nfail(&c, "%s", "ui.progress needs a number from 0 to 1");
				goto bad;
			}
			if (d < 0) d = 0;
			if (d > 1) d = 1;
			o = v_map();
			v_map_set(o, "t", v_str("progress"));
			v_map_set(o, "permille", v_int(lround(d * 1000)));
			if (!opt_line(&c, o, p, "label", 40, "ui.progress: the label"))
				goto bad_o;
		} else if (!strcmp(kind, "list")) {
			if (!(o = list(&c, p)))
				goto bad;
		} else if (!strcmp(kind, "button")) {
			static const char *const keys[] = { "t", "label", "icon", "action", "selected", NULL };
			if (++buttons > DESC_MAX_BUTTONS) {
				nfail(&c, "%s", "at most two ui.button");
				goto bad;
			}
			if (!only_keys(&c, p, keys, "ui.button"))
				goto bad;
			o = v_map();
			v_map_set(o, "t", v_str("button"));
			if (!req_line(&c, o, p, "label", 40, "ui.button: the label"))
				goto bad_o;
			const struct value *ic = v_map_get(p, "icon");
			if (ic && ic->t != V_NULL) {
				bool okic = ic->t == V_STR && ic->u.s.len >= 1 && ic->u.s.len <= 64;
				for (size_t k = 0; okic && k < ic->u.s.len; k++) {
					char ch = ic->u.s.p[k];
					okic = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
				}
				if (!okic) {
					nfail(&c, "%s", "ui.button: the icon must be an icon name");
					goto bad_o;
				}
				v_map_set(o, "icon", v_dup(ic));
			}
			struct value *a = action(&c, v_map_get(p, "action"));
			if (!a)
				goto bad_o;
			v_map_set(o, "action", a);
			const struct value *sel = v_map_get(p, "selected");
			if (sel && sel->t != V_NULL) {
				if (!edit) {
					nfail(&c, "%s", "selected works only in widget.edit.lua");
					goto bad_o;
				}
				if (is_true(sel))
					v_map_set(o, "selected", v_bool(true));
			}
		} else if (!strcmp(kind, "image")) {
			if (++images > DESC_MAX_IMAGES) {
				nfail(&c, "%s", "at most two ui.image");
				goto bad;
			}
			if (!(o = image(&c, p)))
				goto bad;
		} else {
			nfail(&c, "%s", "render returned something that is not a ui primitive");
			goto bad;
		}
		v_arr_push(out, o);
		continue;
bad_o:
		v_free(o);
		goto bad;
	}
	return out;
bad:
	v_free(out);
	return NULL;
}
