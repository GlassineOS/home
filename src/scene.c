// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The scene: Home's header, the page of cards, Arrange's frame, peeks and buttons,
 * and the sheets (app menu, gallery, a widget's settings), painted with
 * GlassineOS/ui in the current voice. Geometry follows the design's Home screen
 * (logical px); every tappable thing is recorded as a hit, every part that can
 * change on its own as a region (home.h).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>

#include "home.h"

#define PAD_X 16
#define PAD_TOP 12
#define PAD_BOTTOM 14
#define HEAD_H 52
#define GAP 10
#define EDIT_X 46
#define EDIT_Y 8

static UiGc G;
static struct home *H;
static ui_col INK, SUB, PAPER;

/* ---- small helpers ------------------------------------------------------------- */

static struct hit *hit_add(struct rect r, enum act act, int a, int b, const char *id, bool focus, double radius)
{
	if (H->nhits >= MAX_HITS)
		return NULL;
	struct hit *t = &H->hits[H->nhits++];
	memset(t, 0, sizeof *t);
	t->r = r;
	t->act = act;
	t->a = a;
	t->b = b;
	t->focus = focus;
	t->radius = radius;
	if (id)
		snprintf(t->id, sizeof t->id, "%s", id);
	return t;
}

static void region_add(struct rect r, enum region_kind k, int card)
{
	if (H->nregions < MAX_REGIONS)
		H->regions[H->nregions++] = (struct region){ r, k, card };
}

static UiFont fpx(const UiFont *f, float px)
{
	return ui_font_px(f, px);
}

static double text_h(const UiFont *f)
{
	double h = 0;
	ui_text_w(&G, f, "Ag", &h);
	return h;
}

static double btn_r(void)
{
	return H->v->btn.r[0] == UI_HALF ? 22 : H->v->btn.r[0];
}

/* a square or pill button of the voice (design: btnBg + btnSh + btnR) */
static void button_box(struct rect r, bool selected)
{
	ui_box(&G, r.x, r.y, r.w, r.h, selected ? &H->v->sel : &H->v->btn);
}

static void icon_button(struct rect r, const char *icon, double px, ui_col fg, bool selected, enum act act, int a, int b,
			bool enabled)
{
	button_box(r, selected);
	ui_icon_material(&G, icon, px, r.x + r.w / 2, r.y + r.h / 2, enabled ? fg : UI_RULE);
	if (enabled)
		hit_add(r, act, a, b, NULL, true, btn_r());
}

/* an outlined control (Arrange's card buttons: 1.5 px border in the colour, no fill) */
static void ctl_button(struct rect r, const char *icon, bool enabled, enum act act, int a)
{
	ui_col c = enabled ? INK : UI_RULE;
	ui_stroke_rrect(&G, r.x, r.y, r.w, r.h, btn_r() > r.h / 2 ? r.h / 2 : btn_r(), 1.5, c, 0);
	ui_icon_material(&G, icon, 20, r.x + r.w / 2, r.y + r.h / 2, c);
	if (enabled)
		hit_add(r, act, a, 0, NULL, true, btn_r());
}

static void rounded_clip(cairo_t *cr, struct rect r, double rad)
{
	double x = r.x, y = r.y, w = r.w, h = r.h;
	if (rad > w / 2)
		rad = w / 2;
	if (rad > h / 2)
		rad = h / 2;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - rad, y + rad, rad, -M_PI / 2, 0);
	cairo_arc(cr, x + w - rad, y + h - rad, rad, 0, M_PI / 2);
	cairo_arc(cr, x + rad, y + h - rad, rad, M_PI / 2, M_PI);
	cairo_arc(cr, x + rad, y + rad, rad, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
	cairo_clip(cr);
}

/* ---- names ----------------------------------------------------------------------- */

const char *card_name(const struct home *h, const struct card *c, char *buf, size_t len)
{
	switch (c->nat) {
	case NAT_FAV:
		return "Favourite apps";
	case NAT_ALL:
		return "All apps";
	case NAT_CAT: {
		const struct value *cat = c->props ? v_map_get(c->props, "category") : NULL;
		if (cat && cat->t == V_STR && cat->u.s.len) {
			snprintf(buf, len, "%s", cat->u.s.p);
			return buf;
		}
		return "App category";
	}
	case NAT_NONE:
		break;
	}
	return c->m ? c->m->name : "Missing widget";
}

static const char *card_icon(const struct card *c)
{
	switch (c->nat) {
	case NAT_FAV:
		return "star";
	case NAT_ALL:
		return "apps";
	case NAT_CAT:
		return "category";
	case NAT_NONE:
		break;
	}
	return c->m && c->m->icon ? c->m->icon : "widgets";
}

static const char *cat_icon(const char *cat)
{
	static const struct { const char *c, *i; } m[] = {
		{ "Reading", "menu_book" }, { "Office", "description" }, { "Education", "school" },
		{ "Games", "sports_esports" }, { "Multimedia", "music_note" }, { "Internet", "public" },
		{ "System", "settings" },
	};
	for (size_t i = 0; i < sizeof m / sizeof *m; i++)
		if (!strcmp(m[i].c, cat))
			return m[i].i;
	return "apps";
}

/* ---- the header -------------------------------------------------------------------- */

void home_status(const struct home *h, char *title, size_t tlen, char *sub, size_t slen)
{
	time_t t = (time_t)home_now(h);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(title, tlen, "%A %-d %B", &tm);
	char hm[16];
	strftime(hm, sizeof hm, "%H:%M", &tm);
	int bat = -1;
	const char *dir = getenv("GLASSINE_HOME_POWER");
	if (!dir || !*dir)
		dir = "/sys/class/power_supply";
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && bat < 0 && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char p[1024], ty[32] = "";
		snprintf(p, sizeof p, "%s/%s/type", dir, e->d_name);
		FILE *f = fopen(p, "re");
		if (!f)
			continue;
		if (!fgets(ty, sizeof ty, f))
			ty[0] = 0;
		fclose(f);
		if (strncmp(ty, "Battery", 7))
			continue;
		snprintf(p, sizeof p, "%s/%s/capacity", dir, e->d_name);
		f = fopen(p, "re");
		if (f) {
			if (fscanf(f, "%d", &bat) != 1)
				bat = -1;
			fclose(f);
		}
	}
	if (d)
		closedir(d);
	if (bat >= 0)
		snprintf(sub, slen, "%s · battery %d %%", hm, bat);
	else
		snprintf(sub, slen, "%s", hm);
}

static double paint_header(void)
{
	const UiVoice *v = H->v;
	double cy = PAD_TOP + HEAD_H / 2.0, xr = H->W - PAD_X;
	/* Arrange / Done */
	struct rect b = { xr - 48, cy - 24, 48, 48 };
	if (H->edit)
		icon_button(b, "check", 24, v->selFg, true, A_DONE, -1, 0, true);
	else
		icon_button(b, "dashboard_customize", 24, INK, false, A_ARRANGE, -1, 0, true);
	xr = b.x - 8;
	/* the pager */
	if (H->npages > 1) {
		char lbl[32];
		snprintf(lbl, sizeof lbl, "%d/%d", H->page + 1, H->npages);
		double lw = ui_text_w(&G, &v->pageFont, lbl, NULL);
		if (lw < 34)
			lw = 34;
		struct rect nx = { xr - 44, cy - 22, 44, 44 };
		icon_button(nx, "chevron_right", 24, INK, false, A_PAGE_NEXT, -1, 0, H->page + 1 < H->npages);
		double lx = nx.x - 4 - lw;
		double th = text_h(&v->pageFont);
		ui_text(&G, &v->pageFont, lbl, lx + lw / 2, cy - th / 2, 1, 0, INK, NULL);
		struct rect pv = { lx - 4 - 44, cy - 22, 44, 44 };
		icon_button(pv, "chevron_left", 24, INK, false, A_PAGE_PREV, -1, 0, H->page > 0);
		xr = pv.x - 8;
	}
	char title[96], sub[128];
	if (H->edit) {
		snprintf(title, sizeof title, "Arrange Home");
		snprintf(sub, sizeof sub, "Swap on any shared edge · tap a page at the side to turn to it");
	} else {
		home_status(H, title, sizeof title, sub, sizeof sub);
	}
	UiFont df = fpx(&v->dateFont, 13);
	double h1 = text_h(&v->headFont), h2 = text_h(&df), top = cy - (h1 + 2 + h2) / 2, mw = xr - PAD_X;
	ui_text(&G, &v->headFont, title, PAD_X, top, 0, mw, INK, NULL);
	ui_text(&G, &df, sub, PAD_X, top + h1 + 2, 0, mw, SUB, NULL);
	double ry = PAD_TOP + HEAD_H + 8;
	ui_hline(&G, PAD_X, ry, H->W - 2 * PAD_X, v->headRuleW, v->headRuleC, 0);
	region_add((struct rect){ 0, 0, H->W, ry + v->headRuleW }, R_HEADER, -1);
	return ry + v->headRuleW;
}

/* ---- layout of a page -------------------------------------------------------------- */

struct grid_geo { double gx, gy, gw, gh, cw, rh; int C, R; };

static struct grid_geo geo(double top, double bottom, bool edit, double xoff)
{
	struct grid g;
	g = pack_grid(H->orient);
	struct grid_geo o;
	o.C = 4 * g.lanes;
	o.R = g.rows;
	o.gx = PAD_X + (edit ? EDIT_X : 0) + xoff;
	o.gw = H->W - 2 * PAD_X - (edit ? 2 * EDIT_X : 0);
	o.gy = top + (edit ? EDIT_Y : 0);
	o.gh = bottom - top - (edit ? 2 * EDIT_Y : 0);
	o.cw = (o.gw - (o.C - 1) * GAP) / o.C;
	o.rh = (o.gh - (o.R - 1) * GAP) / o.R;
	return o;
}

static struct rect cell_rect(const struct grid_geo *g, const struct pack_pos *p)
{
	int x0 = p->lane * 4 + p->col;
	return (struct rect){ g->gx + x0 * (g->cw + GAP), g->gy + p->row * (g->rh + GAP),
			      p->cols * g->cw + (p->cols - 1) * GAP, p->rows * g->rh + (p->rows - 1) * GAP };
}

/* ---- image slots ----------------------------------------------------------------------- */

static void paint_slot(struct card *c, int k, const struct value *it, struct rect r)
{
	region_add(r, R_SLOT, (int)(c - H->cards));
	img_request(H, c, k, it, (int)lround(r.w), (int)lround(r.h));
	const struct img *im = &c->img[k];
	if (im->surf) {
		cairo_t *cr = G.cr;
		if (G.paint) {
			cairo_save(cr);
			rounded_clip(cr, r, 4);
			cairo_translate(cr, r.x, r.y);
			cairo_scale(cr, 1.0 / H->S, 1.0 / H->S);
			cairo_set_source_surface(cr, im->surf, 0, 0);
			cairo_paint(cr);
			cairo_restore(cr);
		}
		ui_stroke_rrect(&G, r.x, r.y, r.w, r.h, 4, 1.5, INK, 0);
		return;
	}
	ui_stroke_rrect(&G, r.x, r.y, r.w, r.h, 4, 1.5, SUB, 1);
	const struct value *alt = v_map_get(it, "alt");
	if (alt && r.w > 16 && r.h > 12) {
		UiFont f = fpx(&H->v->dateFont, 11);
		double th = ui_text_wrap(&G, &f, alt->u.s.p, 0, 0, r.w - 8, (int)((r.h - 8) / (11 * 1.25)) + 1, 0, 0);
		if (th > r.h - 8)
			th = r.h - 8;
		cairo_save(G.cr);
		cairo_rectangle(G.cr, r.x + 2, r.y + 2, r.w - 4, r.h - 4);
		cairo_clip(G.cr);
		ui_text_wrap(&G, &f, alt->u.s.p, r.x + 4, r.y + (r.h - th) / 2, r.w - 8,
			     (int)((r.h - 8) / (11 * 1.25)) + 1, 0, SUB);
		cairo_restore(G.cr);
	}
}

/* ---- a script widget's body ----------------------------------------------------------- */

static const char *str_of(const struct value *m, const char *k)
{
	const struct value *v = m ? v_map_get(m, k) : NULL;
	return v && v->t == V_STR ? v->u.s.p : NULL;
}

static void ago(long long then, long long now, char *out, size_t len)
{
	long long d = now - then;
	if (d < 60)
		snprintf(out, len, "just now");
	else if (d < 3600)
		snprintf(out, len, "%lld min ago", d / 60);
	else if (d < 86400)
		snprintf(out, len, "%lld h ago", d / 3600);
	else if (d < 60 * 86400)
		snprintf(out, len, "%lld d ago", d / 86400);
	else if (d < 730 * 86400)
		snprintf(out, len, "%lld months ago", d / (30 * 86400));
	else
		snprintf(out, len, "%lld years ago", d / (365 * 86400));
}

static void source_label(const struct card *c, char *out, size_t len)
{
	if (!c->ce) {
		snprintf(out, len, "Built into Home");
		return;
	}
	const char *id = strchr(c->ce->ref, '/') + 1;
	if (!strncmp(c->ce->ref, "builtin/", 8))
		snprintf(out, len, "Built in · %s", id);
	else if (!strncmp(c->ce->ref, "system/", 7))
		snprintf(out, len, "From an app · %s", id);
	else
		snprintf(out, len, "Your widget · %s", id);
}

/* an item's height in the body, as paint_script lays it out (with the gap after it) */
static double item_h(const struct value *it, const struct value *side, const UiFont *tf, const UiFont *nf)
{
	const char *t = str_of(it, "t");
	double hh;
	if (!strcmp(t, "image")) {
		if (it == side)
			return 0;
		hh = (double)v_map_get(it, "h")->u.i;
	} else if (!strcmp(t, "title")) {
		hh = text_h(&H->v->nameFont);
	} else if (!strcmp(t, "text")) {
		hh = text_h(tf);
	} else if (!strcmp(t, "progress")) {
		hh = text_h(nf) > 8 ? text_h(nf) : 8;
	} else if (!strcmp(t, "list")) {
		const struct value *rows = v_map_get(it, "rows");
		hh = rows ? rows->u.a.n * (text_h(tf) + 6) : 0;
	} else if (!strcmp(t, "button")) {
		hh = 36;
	} else {
		return 0;
	}
	return hh + 4;
}

static void paint_script(struct card *c, int ci, struct rect in)
{
	const UiVoice *v = H->v;
	UiFont mf = fpx(&v->dateFont, 11.5), tf = fpx(&v->dateFont, 14), lf = fpx(&v->dateFont, 12), nf = fpx(&v->numFont, 13);
	char meta[300] = "";
	bool own_meta = false; /* the widget's, not Home's (the source in Arrange, a failure) */
	const struct value *d = c->desc;
	if (H->edit) {
		source_label(c, meta, sizeof meta);
	} else if (c->st.paused) {
		snprintf(meta, sizeof meta, "Paused after %d failures · open Add widget to retry", PAUSE_AFTER_FAILURES);
	} else if (c->fail[0] && d) {
		snprintf(meta, sizeof meta, "Not updated: %s", c->fail);
	} else {
		own_meta = true;
		for (size_t i = 0; d && d->t == V_ARR && i < d->u.a.n; i++) {
			const struct value *it = d->u.a.v[i];
			if (strcmp(str_of(it, "t"), "meta"))
				continue;
			const struct value *tm = v_map_get(it, "time");
			char a[40] = "";
			if (tm && tm->t == V_INT)
				ago(tm->u.i, home_now(H), a, sizeof a);
			snprintf(meta, sizeof meta, "%s%s%s", str_of(it, "text"), a[0] ? " " : "", a);
		}
	}
	/* a side image */
	int side_k = -1;
	const struct value *side = NULL;
	for (size_t i = 0, k = 0; d && d->t == V_ARR && i < d->u.a.n; i++) {
		const struct value *it = d->u.a.v[i];
		if (strcmp(str_of(it, "t"), "image"))
			continue;
		if (str_of(it, "side")) {
			side = it;
			side_k = (int)k;
			break;
		}
		k++;
	}
	/* what fits: when the items are taller than the body, plain text lines go first, from
	 * the last up, then the meta line, so a button isn't cut off; the rest is clipped */
	double mh = meta[0] ? text_h(&mf) + 6 : 0;
	bool skip[DESC_MAX_ITEMS + 1] = { false };
	if (d && d->t == V_ARR) {
		double total = 0, sh = side ? (double)v_map_get(side, "h")->u.i : 0;
		for (size_t i = 0; i < d->u.a.n && i <= DESC_MAX_ITEMS; i++)
			total += item_h(d->u.a.v[i], side, &tf, &nf);
		total -= 4;
		for (size_t i = d->u.a.n; total > in.h - mh && i-- > 0;)
			if (i <= DESC_MAX_ITEMS && !strcmp(str_of(d->u.a.v[i], "t"), "text")) {
				skip[i] = true;
				total -= item_h(d->u.a.v[i], side, &tf, &nf);
			}
		if (own_meta && total > in.h - mh && mh > 0 && total <= in.h && sh <= in.h)
			mh = 0, meta[0] = 0;
	}
	double bottom = in.y + in.h;
	if (meta[0]) {
		ui_text(&G, &mf, meta, in.x, bottom - text_h(&mf), 0, in.w, SUB, NULL);
		bottom -= mh;
	}
	struct rect body = { in.x, in.y, in.w, bottom - in.y };
	if (body.h <= 0)
		return;
	cairo_save(G.cr);
	/* the clip leaves room for a button's shadow */
	cairo_rectangle(G.cr, body.x - 4, body.y, body.w + 8, body.h + 4);
	cairo_clip(G.cr);
	if (!d) {
		if (!c->m) {
			char ref[100];
			snprintf(ref, sizeof ref, "%s", H->conf.w[ci].widget);
			ui_text(&G, &v->nameFont, "Widget not found", body.x, body.y, 0, body.w, INK, NULL);
			ui_text(&G, &tf, ref, body.x, body.y + text_h(&v->nameFont) + 4, 0, body.w, SUB, NULL);
		} else if (c->fail[0]) {
			ui_text(&G, &tf, "Couldn't show this widget", body.x, body.y, 0, body.w, INK, NULL);
			ui_text(&G, &tf, c->fail, body.x, body.y + text_h(&tf) + 4, 0, body.w, SUB, NULL);
		}
		cairo_restore(G.cr);
		return;
	}
	double cx = body.x, cw = body.w;
	if (side) {
		double w = (double)v_map_get(side, "w")->u.i, hh = (double)v_map_get(side, "h")->u.i;
		if (w > body.w / 2)
			w = floor(body.w / 2);
		if (hh > body.h)
			hh = body.h;
		bool right = !strcmp(str_of(side, "side"), "right");
		struct rect r = { right ? body.x + body.w - w : body.x, body.y, w, hh };
		paint_slot(c, side_k, side, r);
		cw = body.w - w - 12;
		if (!right)
			cx = body.x + w + 12;
	}
	double y = body.y;
	int img_k = 0;
	for (size_t i = 0; i < d->u.a.n; i++) {
		const struct value *it = d->u.a.v[i];
		const char *t = str_of(it, "t");
		double hh = 0;
		if (i <= DESC_MAX_ITEMS && skip[i])
			continue;
		if (!strcmp(t, "image")) {
			int my_k = img_k++;
			if (it == side)
				continue;
			hh = (double)v_map_get(it, "h")->u.i;
			if (hh > body.y + body.h - y)
				hh = body.y + body.h - y;
			if (hh < 4)
				continue;
			paint_slot(c, my_k, it, (struct rect){ cx, y, cw, hh });
		} else if (!strcmp(t, "title")) {
			ui_text(&G, &v->nameFont, str_of(it, "text"), cx, y, 0, cw, INK, &hh);
		} else if (!strcmp(t, "text")) {
			const struct value *soft = v_map_get(it, "soft");
			ui_text(&G, &tf, str_of(it, "text"), cx, y, 0, cw, soft ? SUB : INK, &hh);
		} else if (!strcmp(t, "progress")) {
			const char *label = str_of(it, "label");
			long long pm = v_map_get(it, "permille")->u.i;
			double lw = 0, lh = text_h(&nf);
			if (label)
				lw = ui_text_w(&G, &nf, label, NULL);
			hh = lh > 8 ? lh : 8;
			double bw = cw - (label ? lw + 10 : 0), by = y + (hh - 8) / 2;
			if (bw > 8) {
				ui_rrect(&G, cx, by, bw * pm / 1000.0, 8, 4, INK);
				ui_stroke_rrect(&G, cx, by, bw, 8, 4, 1.5, INK, 0);
			}
			if (label)
				ui_text(&G, &nf, label, cx + cw, y + (hh - lh) / 2, 2, 0, INK, NULL);
		} else if (!strcmp(t, "list")) {
			const struct value *rows = v_map_get(it, "rows");
			double lh = text_h(&tf);
			for (size_t r = 0; rows && r < rows->u.a.n; r++) {
				const struct value *row = rows->u.a.v[r];
				const char *m = str_of(row, "meta");
				double mw = m ? ui_text_w(&G, &lf, m, NULL) : 0;
				ui_text(&G, &tf, str_of(row, "label"), cx, y + hh + 3, 0, cw - (m ? mw + 8 : 0), INK, NULL);
				if (m)
					ui_text(&G, &lf, m, cx + cw, y + hh + 3 + (lh - text_h(&lf)) * 0.75, 2, 0, SUB, NULL);
				hh += lh + 6;
				ui_hline(&G, cx, y + hh - 1, cw, 1, UI_RULE, 2);
			}
		} else if (!strcmp(t, "button")) {
			if (y + 36 > body.y + body.h + 4) /* the clip's room below */
				break; /* a button shows whole or not at all (Arrange's smaller cards) */
			const char *label = str_of(it, "label"), *icon = str_of(it, "icon");
			double lw = ui_text_w(&G, &v->titleFont, label, NULL);
			double bw = 12 + (icon ? 26 : 0) + lw + 12;
			if (bw > cw)
				bw = cw;
			struct rect b = { cx, y, bw, 36 };
			bool sel = v_map_get(it, "selected") != NULL;
			button_box(b, sel);
			ui_col fg = sel ? v->selFg : INK;
			double tx = cx + 12;
			if (icon) {
				ui_icon_material(&G, icon, 20, tx + 10, y + 18, fg);
				tx += 26;
			}
			ui_text(&G, &v->titleFont, label, tx, y + (36 - text_h(&v->titleFont)) / 2, 0, cx + bw - 12 - tx, fg, NULL);
			if (!H->edit)
				hit_add(b, A_WBUTTON, ci, (int)i, NULL, true, btn_r());
			hh = 36;
		} else {
			continue; /* meta: drawn at the bottom */
		}
		y += hh + 4;
	}
	cairo_restore(G.cr);
}

/* ---- an app widget's body --------------------------------------------------------- */

static size_t card_apps(const struct card *c, const struct app **out, size_t max)
{
	size_t n = 0;
	const struct apps *l = &H->apps;
	if (c->nat == NAT_FAV) {
		for (size_t i = 0; i < H->conf.nfav && n < max; i++) {
			const struct app *a = apps_find(l, H->conf.favourites[i]);
			if (a)
				out[n++] = a;
		}
		return n;
	}
	const struct value *cat = c->nat == NAT_CAT ? v_map_get(c->props, "category") : NULL;
	for (size_t i = 0; i < l->n && n < max; i++)
		if (c->nat == NAT_ALL || (cat && cat->t == V_STR && !strcmp(l->a[i].cat, cat->u.s.p)))
			out[n++] = &l->a[i];
	return n;
}

static int app_rows(double card_h)
{
	int r = (int)floor((card_h - 60) / 94);
	return r < 1 ? 1 : r;
}

static void paint_apps(struct card *c, int ci, struct rect in, double card_h, struct rect head)
{
	const UiVoice *v = H->v;
	const struct app **list = malloc((H->apps.n + 1) * sizeof *list);
	if (!list)
		return;
	size_t n = card_apps(c, list, H->apps.n + 1);
	int rows = app_rows(card_h), per = rows * 4, npg = n ? (int)((n + per - 1) / per) : 1;
	if (c->apage >= npg)
		c->apage = npg - 1;
	if (c->apage < 0)
		c->apage = 0;
	/* its own pager, in its header */
	if (!H->edit && npg > 1) {
		double cy = head.y + head.h / 2, xr = head.x + head.w;
		char lbl[24];
		snprintf(lbl, sizeof lbl, "%d/%d", c->apage + 1, npg);
		UiFont pf = fpx(&v->pageFont, 13);
		double lw = ui_text_w(&G, &pf, lbl, NULL);
		if (lw < 30)
			lw = 30;
		struct rect nx = { xr - 40, cy - 20, 40, 40 };
		icon_button(nx, "chevron_right", 22, INK, false, A_APPS_NEXT, ci, 0, c->apage + 1 < npg);
		ui_text(&G, &pf, lbl, nx.x - 4 - lw / 2, cy - text_h(&pf) / 2, 1, 0, INK, NULL);
		struct rect pv = { nx.x - 8 - lw - 40, cy - 20, 40, 40 };
		icon_button(pv, "chevron_left", 22, INK, false, A_APPS_PREV, ci, 0, c->apage > 0);
	}
	if (!n) {
		UiFont tf = fpx(&v->dateFont, 14);
		char msg[120];
		if (c->nat == NAT_FAV)
			snprintf(msg, sizeof msg, "Hold an app and add it to your favourites");
		else if (c->nat == NAT_CAT)
			snprintf(msg, sizeof msg, "No apps in this category");
		else
			snprintf(msg, sizeof msg, "No apps installed");
		ui_text(&G, &tf, msg, in.x, in.y, 0, in.w, SUB, NULL);
		free(list);
		return;
	}
	cairo_t *cr = G.cr;
	if (H->edit && G.paint)
		cairo_push_group(cr);
	double tw = (in.w - 3 * 4) / 4, th = (in.h - (rows - 1) * 4) / rows;
	UiFont nf = fpx(&v->appNameFont, 13);
	double nh = text_h(&nf), oh = text_h(&v->appCatFont);
	for (int k = 0; k < per && (size_t)(c->apage * per + k) < n; k++) {
		const struct app *a = list[c->apage * per + k];
		struct rect t = { in.x + (k % 4) * (tw + 4), in.y + (k / 4) * (th + 4), tw, th };
		bool origin = a->twin && th >= 48 + 5 + nh + oh;
		double block = 48 + 5 + nh + (origin ? oh : 0), top = t.y + (th - block) / 2;
		struct rect ib = { t.x + (tw - 48) / 2, top, 48, 48 };
		ui_box(&G, ib.x, ib.y, 48, 48, &v->btn);
		if (!(a->icon[0] && ui_icon(&G, a->icon, 32, ib.x + 24, ib.y + 24, INK)))
			ui_icon_material(&G, cat_icon(a->cat), 26, ib.x + 24, ib.y + 24, INK);
		ui_text(&G, &nf, a->name, t.x + tw / 2, top + 53, 1, tw, INK, NULL);
		if (origin)
			ui_text(&G, &v->appCatFont, a->origin[0] ? a->origin : "Package", t.x + tw / 2, top + 53 + nh, 1, tw, SUB, NULL);
		if (H->sheet == SHEET_MENU && !strcmp(H->menu_app, a->id))
			ui_stroke_rrect(&G, t.x, t.y, t.w, t.h, btn_r(), 2, INK, 0);
		if (!H->edit)
			hit_add(t, A_APP, ci, 0, a->id, true, btn_r());
	}
	if (H->edit && G.paint) {
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, 0.35);
	}
	free(list);
}

/* ---- a card ------------------------------------------------------------------------- */

static void paint_card(int ci, struct rect r)
{
	const UiVoice *v = H->v;
	struct card *c = &H->cards[ci];
	region_add(r, R_CARD, ci);
	UiBox box = { .bg = v->card.bg ? v->card.bg : PAPER, .bw = H->edit ? 2 : 1.5, .bc = INK, .dashed = H->edit };
	for (int i = 0; i < 4; i++)
		box.r[i] = (float)btn_r();
	ui_box(&G, r.x, r.y, r.w, r.h, &box);
	struct rect in = { r.x + 12, r.y + 8, r.w - 24, r.h - 16 };
	char nb[128];
	const char *name = card_name(H, c, nb, sizeof nb);
	/* the header row: icon, name, then the pager or Arrange's buttons */
	bool tall = H->edit || (c->nat != NAT_NONE);
	double hh = tall ? 40 : 24, cy = in.y + hh / 2, xr = in.x + in.w;
	struct rect head = { in.x, in.y, in.w, hh };
	if (H->edit) {
		bool settings = card_has_settings(H, c);
		bool sizes = c->nat != NAT_NONE || (c->m && c->m->nsizes > 1);
		const char *size_icon = c->nat != NAT_NONE ? "height"
			: H->conf.w[ci].size == SIZE_WIDE ? "close_fullscreen" : "open_in_full";
		struct rect b = { xr - 40, cy - 20, 40, 40 };
		ctl_button(b, "close", true, A_CTL_REMOVE, ci);
		b.x -= 46;
		ctl_button(b, size_icon, sizes, A_CTL_SIZE, ci);
		if (settings) {
			b.x -= 46;
			ctl_button(b, "tune", true, A_CTL_SETTINGS, ci);
		}
		xr = b.x - 6;
	} else if (c->nat != NAT_NONE) {
		xr -= 40 + 8 + 30 + 4 + 40 + 6; /* room for the pager (painted with the apps) */
	}
	ui_icon_material(&G, card_icon(c), 18, in.x + 9, cy, INK);
	double lh = text_h(&v->lbl);
	ui_text(&G, &v->lbl, name, in.x + 24, cy - lh / 2, 0, xr - in.x - 24, v->lblC, NULL);
	struct rect bodyr = { in.x, in.y + hh + 6, in.w, in.h - hh - 6 };
	if (c->nat != NAT_NONE)
		paint_apps(c, ci, bodyr, r.h, head);
	else
		paint_script(c, ci, bodyr);
}

/* ---- Arrange: peeks, swaps, page swaps ----------------------------------------------- */

static void paint_peek(int page, const struct pack_pos *pos, size_t n, double left, double top, double bottom,
		       struct rect clip)
{
	struct grid_geo g = geo(top, bottom, true, 0);
	g.gx = left;
	cairo_save(G.cr);
	cairo_rectangle(G.cr, clip.x, clip.y, clip.w, clip.h);
	cairo_clip(G.cr);
	ui_stroke_rrect(&G, g.gx - 7.5, g.gy - 7.5, g.gw + 15, g.gh + 15, 4, 1.5, SUB, 1);
	for (size_t i = 0; i < n; i++) {
		if (pos[i].page != page)
			continue;
		struct rect r = cell_rect(&g, &pos[i]);
		ui_hatch(&G, r.x, r.y, r.w, r.h, btn_r(), 135, 1, 7, UI_RULE);
		ui_stroke_rrect(&G, r.x, r.y, r.w, r.h, btn_r(), 1.5, SUB, 0);
	}
	cairo_restore(G.cr);
	struct rect fr = { g.gx - 7.5, g.gy - 7.5, g.gw + 15, g.gh + 15 };
	double x0 = fr.x > clip.x ? fr.x : clip.x, x1 = fr.x + fr.w < clip.x + clip.w ? fr.x + fr.w : clip.x + clip.w;
	if (x1 - x0 >= 8) {
		hit_add((struct rect){ x0, fr.y, x1 - x0, fr.h }, A_PEEK, page, 0, NULL, true, 4);
	}
}

static void swap_button(double cx, double cy, bool vertical, int a, int b)
{
	struct rect r = { cx - 22, cy - 22, 44, 44 };
	ui_rrect(&G, r.x - 3, r.y - 3, 50, 50, btn_r() + 3, PAPER);
	ui_box(&G, r.x, r.y, r.w, r.h, &H->v->sel);
	ui_icon_material(&G, vertical ? "swap_vert" : "swap_horiz", 24, cx, cy, H->v->selFg);
	hit_add(r, A_SWAP, a, b, NULL, true, btn_r());
}

static void page_swap_button(double cx, double cy, int a, int b)
{
	struct rect r = { cx - 22, cy - 22, 44, 44 };
	ui_rrect(&G, r.x - 3, r.y - 3, 50, 50, 25, PAPER);
	ui_rrect(&G, r.x, r.y, 44, 44, 22, INK);
	ui_rrect(&G, r.x + 2, r.y + 2, 40, 40, 20, PAPER);
	ui_icon_material(&G, "sync_alt", 24, cx, cy, INK);
	hit_add(r, A_PAGE_SWAP, a, b, NULL, true, 22);
}

/* ---- the page ------------------------------------------------------------------------ */

static void paint_page(double top, double bottom)
{
	size_t n = H->conf.n;
	int *sizes = malloc((n + 1) * sizeof *sizes);
	struct pack_pos *pos = calloc(n + 1, sizeof *pos);
	if (!sizes || !pos) {
		free(sizes);
		free(pos);
		return;
	}
	for (size_t i = 0; i < n; i++)
		sizes[i] = H->conf.w[i].size;
	int real = pack(H->orient, sizes, n, pos);
	int pages = real;
	struct pack_pos add = { 0 };
	if (H->edit) {
		sizes[n] = SIZE_HALF;
		struct pack_pos *p2 = calloc(n + 1, sizeof *p2);
		if (p2) {
			pages = pack(H->orient, sizes, n + 1, p2);
			add = p2[n];
			free(p2);
		}
	}
	if (pages < 1)
		pages = 1;
	H->npages = pages;
	if (H->page >= pages)
		H->page = pages - 1;
	region_add((struct rect){ 0, top - GAP, H->W, bottom - top + 2 * GAP }, R_PAGE, -1);
	struct grid_geo g = geo(top, bottom, H->edit, 0);
	if (H->edit) {
		struct rect clip = { PAD_X, top, H->W - 2 * PAD_X, bottom - top };
		if (H->page > 0)
			paint_peek(H->page - 1, pos, n, PAD_X + 3 * EDIT_X - (H->W - 2 * PAD_X) - 18, top, bottom, clip);
		if (H->page + 1 < pages)
			paint_peek(H->page + 1, pos, n, H->W - PAD_X - EDIT_X + 18, top, bottom, clip);
		ui_stroke_rrect(&G, g.gx - 7.5, g.gy - 7.5, g.gw + 15, g.gh + 15, 4, 1.5, INK, 0);
	}
	for (size_t i = 0; i < n; i++)
		if (pos[i].page == H->page)
			paint_card((int)i, cell_rect(&g, &pos[i]));
	if (H->edit && add.page == H->page) {
		struct rect r = cell_rect(&g, &add);
		ui_stroke_rrect(&G, r.x, r.y, r.w, r.h, btn_r(), 2, INK, 1);
		double lw = ui_text_w(&G, &H->v->titleFont, "Add widget", NULL), bw = 26 + 8 + lw;
		ui_icon_material(&G, "add", 26, r.x + (r.w - bw) / 2 + 13, r.y + r.h / 2, INK);
		ui_text(&G, &H->v->titleFont, "Add widget", r.x + (r.w - bw) / 2 + 34, r.y + (r.h - text_h(&H->v->titleFont)) / 2,
			0, 0, INK, NULL);
		hit_add(r, A_ADD_CARD, -1, 0, NULL, true, btn_r());
	}
	if (H->edit) {
		/* swaps on shared edges, only where both still fit on this page */
		for (size_t a = 0; a < n; a++)
			for (size_t b = 0; b < n; b++) {
				if (a == b || pos[a].page != H->page || pos[b].page != H->page)
					continue;
				int ax0 = pos[a].lane * 4 + pos[a].col, ax1 = ax0 + pos[a].cols, ay0 = pos[a].row, ay1 = ay0 + pos[a].rows;
				int bx0 = pos[b].lane * 4 + pos[b].col, bx1 = bx0 + pos[b].cols, by0 = pos[b].row, by1 = by0 + pos[b].rows;
				int oy0 = ay0 > by0 ? ay0 : by0, oy1 = ay1 < by1 ? ay1 : by1;
				int ox0 = ax0 > bx0 ? ax0 : bx0, ox1 = ax1 < bx1 ? ax1 : bx1;
				bool vedge = ax1 == bx0 && oy0 < oy1, hedge = ay1 == by0 && ox0 < ox1;
				if ((!vedge && !hedge) || !pack_can_swap(H->orient, sizes, n, a, b))
					continue;
				if (vedge)
					swap_button(g.gx + ax1 * (g.cw + GAP) - GAP / 2.0,
						    (g.gy + oy0 * (g.rh + GAP) + g.gy + oy1 * (g.rh + GAP) - GAP) / 2, false, (int)a, (int)b);
				if (hedge)
					swap_button((g.gx + ox0 * (g.cw + GAP) + g.gx + ox1 * (g.cw + GAP) - GAP) / 2,
						    g.gy + ay1 * (g.rh + GAP) - GAP / 2.0, true, (int)a, (int)b);
			}
		double my = (top + bottom) / 2;
		if (H->page > 0 && H->page < real)
			page_swap_button(PAD_X + EDIT_X - 9, my, H->page, H->page - 1);
		if (H->page < real - 1)
			page_swap_button(H->W - PAD_X - EDIT_X + 9, my, H->page, H->page + 1);
	}
	free(sizes);
	free(pos);
}

/* the page count (with Arrange's Add widget card), before the header shows it */
static void count_pages(void)
{
	size_t n = H->conf.n;
	int *sizes = malloc((n + 1) * sizeof *sizes);
	struct pack_pos *pos = calloc(n + 1, sizeof *pos);
	int pages = 1;
	if (sizes && pos) {
		for (size_t i = 0; i < n; i++)
			sizes[i] = H->conf.w[i].size;
		if (H->edit)
			sizes[n++] = SIZE_HALF;
		pages = pack(H->orient, sizes, n, pos);
	}
	free(sizes);
	free(pos);
	H->npages = pages < 1 ? 1 : pages;
	if (H->page >= H->npages)
		H->page = H->npages - 1;
	if (H->page < 0)
		H->page = 0;
}

/* ---- sheets ----------------------------------------------------------------------------- */

struct srow {
	const char *icon;          /* material */
	const char *app_icon;      /* or an app's icon */
	char name[160];
	char sub[400];
	const char *btn_icon;      /* NULL: no button */
	bool btn_dim, selected;
	enum act act;              /* the row's action (A_NONE: none) */
	int a, b;
	bool act_on_button;        /* only the button is the target */
};

static double sheet_row_h(const struct srow *r, double w, int clamp, double minh, const UiFont *sf)
{
	double nh = text_h(&H->v->titleFont);
	double tw = w - 28 - 12 - (r->btn_icon ? 44 + 12 : 0);
	double sh = r->sub[0] ? ui_text_wrap(&G, sf, r->sub, 0, 0, tw, clamp, 1.35, 0) : 0;
	double h = 6 + nh + (r->sub[0] ? 2 + sh : 0) + 6;
	if (r->btn_icon && h < 44 + 12)
		h = 44 + 12;
	return h < minh ? minh : h;
}

static void paint_sheet_row(const struct srow *r, struct rect box, int clamp, const UiFont *sf)
{
	const UiVoice *v = H->v;
	double nh = text_h(&v->titleFont);
	double tw = box.w - 28 - 12 - (r->btn_icon ? 44 + 12 : 0);
	double sh = r->sub[0] ? ui_text_wrap(&G, sf, r->sub, 0, 0, tw, clamp, 1.35, 0) : 0;
	double th = nh + (r->sub[0] ? 2 + sh : 0), ty = box.y + (box.h - th) / 2, cy = box.y + box.h / 2;
	if (r->app_icon && ui_icon(&G, r->app_icon, 24, box.x + 14, cy, INK))
		;
	else if (r->icon)
		ui_icon_material(&G, r->icon, 24, box.x + 14, cy, INK);
	ui_text(&G, &v->titleFont, r->name, box.x + 40, ty, 0, tw, INK, NULL);
	if (r->sub[0])
		ui_text_wrap(&G, sf, r->sub, box.x + 40, ty + nh + 2, tw, clamp, 1.35, SUB);
	ui_hline(&G, box.x, box.y + box.h - v->sepSoftW, box.w, v->sepSoftW, v->sepSoftC, 0);
	if (r->btn_icon) {
		struct rect b = { box.x + box.w - 44, cy - 22, 44, 44 };
		button_box(b, r->selected);
		ui_icon_material(&G, r->btn_icon, 22, b.x + 22, b.y + 22, r->btn_dim ? SUB : r->selected ? v->selFg : INK);
		if (r->act != A_NONE && r->act_on_button)
			hit_add(b, r->act, r->a, r->b, NULL, true, btn_r());
	}
	if (r->act != A_NONE && !r->act_on_button)
		hit_add(box, r->act, r->a, r->b, NULL, true, 4);
}

static void refresh_text(const struct manifest *m, char *out, size_t len)
{
	char every[64] = "";
	if (m->every_s) {
		long s = m->every_s;
		if (s % 86400 == 0)
			snprintf(every, sizeof every, s == 86400 ? "every day" : "every %ld days", s / 86400);
		else if (s % 3600 == 0)
			snprintf(every, sizeof every, s == 3600 ? "every hour" : "every %ld hours", s / 3600);
		else if (s < 3600)
			snprintf(every, sizeof every, "every %ld minutes", s / 60);
		else
			snprintf(every, sizeof every, "every %ld h %ld min", s / 3600, s % 3600 / 60);
	}
	if (m->on_change && every[0])
		snprintf(out, len, "when its files change and %s", every);
	else if (m->on_change)
		snprintf(out, len, "when its files change");
	else if (every[0])
		snprintf(out, len, "%s", every);
	else
		snprintf(out, len, "when Home appears");
}

static bool on_home(const char *ref)
{
	for (size_t i = 0; i < H->conf.n; i++)
		if (!strcmp(H->conf.w[i].widget, ref))
			return true;
	return false;
}

static int gallery_rows(struct srow *rows, int max)
{
	static const struct { const char *ref, *name, *icon, *blurb; } nat[3] = {
		{ "home/favourites", "Favourite apps", "star", "Your favourites, in your order" },
		{ "home/all-apps", "All apps", "apps", "Every app A–Z, with its own pager" },
		{ "home/app-category", "App category", "category",
		  "The apps of one category, picked in its settings; add as many as you like" },
	};
	int n = 0;
	for (int i = 0; i < 3 && n < max; i++, n++) {
		struct srow *r = &rows[n];
		memset(r, 0, sizeof *r);
		bool has = i < 2 && on_home(nat[i].ref);
		r->icon = nat[i].icon;
		snprintf(r->name, sizeof r->name, "%s", nat[i].name);
		snprintf(r->sub, sizeof r->sub, "Built into Home · %s · 1, 2, 3 or 5 rows tall", nat[i].blurb);
		r->btn_icon = has ? "check" : "add";
		r->btn_dim = has;
		r->act = has ? A_NONE : A_GAL_ADD;
		r->a = i;
		r->act_on_button = true;
	}
	for (size_t e = 0; e < H->cat.n && n < max; e++, n++) {
		const struct catalog_entry *ce = &H->cat.e[e];
		struct srow *r = &rows[n];
		memset(r, 0, sizeof *r);
		const char *id = strchr(ce->ref, '/') + 1;
		char origin[200];
		if (!strncmp(ce->ref, "builtin/", 8))
			snprintf(origin, sizeof origin, "Built in · %s", id);
		else if (!strncmp(ce->ref, "system/", 7))
			snprintf(origin, sizeof origin, "From an app · %s", id);
		else
			snprintf(origin, sizeof origin, "Your widget · ~/.local/share/glassine/widgets/%s", id);
		if (!ce->ok) {
			r->icon = "error";
			snprintf(r->name, sizeof r->name, "%s", id);
			snprintf(r->sub, sizeof r->sub, "%s · can't be added: %s", origin, ce->err);
			continue;
		}
		bool has = on_home(ce->ref);
		const struct manifest *m = &ce->m;
		char when[96];
		refresh_text(m, when, sizeof when);
		const char *sizes = m->nsizes > 1 ? "half or full width" : m->sizes[0] == SIZE_WIDE ? "full width" : "half width";
		r->icon = m->icon;
		snprintf(r->name, sizeof r->name, "%s", m->name);
		/* the description without its full stop: the parts are joined with dots */
		const char *ds = m->description ? m->description : "";
		int dl = (int)strlen(ds);
		if (dl > 0 && ds[dl - 1] == '.')
			dl--;
		snprintf(r->sub, sizeof r->sub, "%s%s%.*s · %s · refreshes %s", origin, dl ? " · " : "", dl, ds, sizes, when);
		r->btn_icon = has ? "check" : "add";
		r->btn_dim = has;
		r->act = has ? A_NONE : A_GAL_ADD;
		r->a = 3 + (int)e;
		r->act_on_button = true;
	}
	return n;
}

static int menu_rows(struct srow *rows, const struct app *a, bool fav)
{
	static const struct { const char *icon, *name, *sub; enum act act; } m[4] = {
		{ "add_to_queue", "Open on a new screen", "The same as a tap", A_MENU_NEW },
		{ "splitscreen", "Open on this screen", "Beside the apps already there; not when it has four", A_MENU_HERE },
		{ "star", "", "Favourite apps shows them in your order", A_MENU_FAV },
		{ "tune", "Configure", "", A_MENU_CONF },
	};
	for (int i = 0; i < 4; i++) {
		struct srow *r = &rows[i];
		memset(r, 0, sizeof *r);
		r->icon = i == 2 && !fav ? "star_outline" : m[i].icon;
		snprintf(r->name, sizeof r->name, "%s", i == 2 ? (fav ? "Remove from favourites" : "Add to favourites") : m[i].name);
		if (i == 3)
			snprintf(r->sub, sizeof r->sub, "Settings › Apps › %s", a ? a->name : H->menu_app);
		else
			snprintf(r->sub, sizeof r->sub, "%s", m[i].sub);
		r->btn_icon = "arrow_forward";
		r->act = m[i].act;
	}
	return 4;
}

/* the settings sheet: the settings script's description as rows and notes */
static int settings_rows(struct srow *rows, int max, const struct value *d, char *title, size_t tlen,
			 char notes[][300], int *nnotes)
{
	int n = 0;
	*nnotes = 0;
	for (size_t i = 0; d && d->t == V_ARR && i < d->u.a.n; i++) {
		const struct value *it = d->u.a.v[i];
		const char *t = str_of(it, "t");
		if (!strcmp(t, "title")) {
			snprintf(title, tlen, "%s", str_of(it, "text"));
		} else if (!strcmp(t, "list")) {
			const struct value *rs = v_map_get(it, "rows");
			for (size_t k = 0; rs && k < rs->u.a.n && n < max; k++) {
				const struct value *row = rs->u.a.v[k];
				struct srow *r = &rows[n++];
				memset(r, 0, sizeof *r);
				bool sel = v_map_get(row, "selected") != NULL, act = v_map_get(row, "action") != NULL;
				r->icon = act || sel ? (sel ? "radio_button_checked" : "radio_button_unchecked") : NULL;
				const char *lb = str_of(row, "label");
				snprintf(r->name, sizeof r->name, "%s", lb ? lb : "");
				snprintf(r->sub, sizeof r->sub, "%s", str_of(row, "meta") ? str_of(row, "meta") : "");
				r->act = act ? A_SET_ROW : A_NONE;
				r->a = (int)i;
				r->b = (int)k;
			}
		} else if (!strcmp(t, "button") && n < max) {
			struct srow *r = &rows[n++];
			memset(r, 0, sizeof *r);
			r->icon = str_of(it, "icon");
			snprintf(r->name, sizeof r->name, "%s", str_of(it, "label"));
			r->btn_icon = "arrow_forward";
			r->selected = v_map_get(it, "selected") != NULL;
			r->act = A_SET_BUTTON;
			r->a = (int)i;
			r->act_on_button = true;
		} else if ((!strcmp(t, "text") || !strcmp(t, "meta")) && *nnotes < 4) {
			snprintf(notes[(*nnotes)++], 300, "%s", str_of(it, "text"));
		} else if (!strcmp(t, "progress") && *nnotes < 4) {
			const char *l = str_of(it, "label");
			snprintf(notes[(*nnotes)++], 300, "%s", l ? l : "");
		}
	}
	return n;
}

static void paint_sheet(void)
{
	const UiVoice *v = H->v;
	struct srow rows[CATALOG_MAX + 8];
	int nrows = 0, cols = 1, clamp = 1, nnotes = 0;
	double minh = 56;
	char title[160] = "", sub[200] = "", notes[4][300];
	const char *icon = "tune", *app_icon = NULL;
	double sw = H->orient == ORIENT_LANDSCAPE ? 600 : 560;
	if (H->sheet == SHEET_MENU) {
		const struct app *a = apps_find(&H->apps, H->menu_app);
		bool fav = home_is_fav(H, H->menu_app);
		nrows = menu_rows(rows, a, fav);
		snprintf(title, sizeof title, "%s", a ? a->name : H->menu_app);
		snprintf(sub, sizeof sub, "%s%s%s", fav ? "Favourite" : "App", a && a->origin[0] ? " · " : "", a ? a->origin : "");
		icon = a ? cat_icon(a->cat) : "apps";
		app_icon = a && a->icon[0] ? a->icon : NULL;
	} else if (H->sheet == SHEET_GALLERY) {
		nrows = gallery_rows(rows, CATALOG_MAX + 8);
		snprintf(title, sizeof title, "Add a widget");
		snprintf(sub, sizeof sub, "Built in, from your apps, or your own");
		icon = "widgets";
		cols = 2;
		clamp = 3;
		minh = 84;
		sw = H->orient == ORIENT_LANDSCAPE ? 880 : H->W - 24;
	} else {
		struct card *c = &H->cards[H->sheet_card];
		char nb[128];
		const char *name = card_name(H, c, nb, sizeof nb);
		snprintf(title, sizeof title, "%s", name);
		snprintf(sub, sizeof sub, "Settings for %s", name);
		nrows = settings_rows(rows, CATALOG_MAX + 8, c->edit_desc, title, sizeof title, notes, &nnotes);
		if (!c->edit_desc && c->fail[0])
			snprintf(notes[nnotes++], 300, "Couldn't show the settings: %s", c->fail);
	}
	if (sw > H->W - 24)
		sw = H->W - 24;
	/* everything outside closes the sheet */
	hit_add((struct rect){ 0, 0, H->W, H->H }, A_SHEET_OUTSIDE, -1, 0, NULL, false, 0);
	UiFont sf = fpx(&v->dateFont, 12.5), hsf = fpx(&v->dateFont, 13);
	double inner = sw - 36, colw = cols == 2 ? (inner - 22) / 2 : inner;
	double headh = 44, maxh = H->H - 24;
	double noteh = 0;
	for (int i = 0; i < nnotes; i++)
		noteh += ui_text_wrap(&G, &sf, notes[i], 0, 0, inner, 6, 0, 0) + 10;
	double avail = maxh - 32 - headh - 8 - v->headRuleW - 10 - noteh;
	/* pages of rows when they don't fit */
	int per_page[64], np = 0, i = 0;
	while (i < nrows && np < 64) {
		double used = 0;
		int start = i;
		while (i < nrows) {
			double rh = sheet_row_h(&rows[i], colw, clamp, minh, &sf);
			if (cols == 2 && i + 1 < nrows) {
				double r2 = sheet_row_h(&rows[i + 1], colw, clamp, minh, &sf);
				rh = rh > r2 ? rh : r2;
			}
			if (used + rh > avail && i > start)
				break;
			used += rh;
			i += cols;
		}
		if (i > nrows)
			i = nrows;
		per_page[np++] = i - start;
	}
	if (!np)
		per_page[np++] = 0;
	H->sheet_pages = np;
	if (H->sheet_page >= np)
		H->sheet_page = np - 1;
	int first = 0;
	for (int p = 0; p < H->sheet_page; p++)
		first += per_page[p];
	int count = per_page[H->sheet_page];
	double bodyh = 0;
	for (int k = 0; k < count; k += cols) {
		double rh = sheet_row_h(&rows[first + k], colw, clamp, minh, &sf);
		if (cols == 2 && k + 1 < count) {
			double r2 = sheet_row_h(&rows[first + k + 1], colw, clamp, minh, &sf);
			rh = rh > r2 ? rh : r2;
		}
		bodyh += rh;
	}
	double shh = 16 + headh + 8 + v->headRuleW + 10 + bodyh + noteh + 16;
	if (shh > maxh)
		shh = maxh;
	struct rect s = { (H->W - sw) / 2, (H->H - shh) / 2, sw, shh };
	region_add((struct rect){ s.x - 12, s.y - 12, s.w + 24, s.h + 24 }, R_SHEET, -1);
	ui_box(&G, s.x, s.y, s.w, s.h, &v->card);
	double x = s.x + 18, y = s.y + 16, cy = y + headh / 2;
	if (!(app_icon && ui_icon(&G, app_icon, 30, x + 15, cy, INK)))
		ui_icon_material(&G, icon, 30, x + 15, cy, INK);
	struct rect cl = { s.x + s.w - 18 - 44, y, 44, 44 };
	icon_button(cl, "close", 22, INK, false, A_SHEET_CLOSE, -1, 0, true);
	double xr = cl.x - 12;
	if (np > 1) {
		char lbl[16];
		snprintf(lbl, sizeof lbl, "%d/%d", H->sheet_page + 1, np);
		struct rect nx = { xr - 44, y, 44, 44 };
		icon_button(nx, "chevron_right", 24, INK, false, A_SHEET_NEXT, -1, 0, H->sheet_page + 1 < np);
		double lw = ui_text_w(&G, &v->pageFont, lbl, NULL);
		lw = lw < 34 ? 34 : lw;
		ui_text(&G, &v->pageFont, lbl, nx.x - 4 - lw / 2, cy - text_h(&v->pageFont) / 2, 1, 0, INK, NULL);
		struct rect pv = { nx.x - 8 - lw - 44, y, 44, 44 };
		icon_button(pv, "chevron_left", 24, INK, false, A_SHEET_PREV, -1, 0, H->sheet_page > 0);
		xr = pv.x - 12;
	}
	double h1 = text_h(&v->nameFont), h2 = text_h(&hsf), ty = cy - (h1 + 2 + h2) / 2;
	ui_text(&G, &v->nameFont, title, x + 42, ty, 0, xr - x - 42, INK, NULL);
	ui_text(&G, &hsf, sub, x + 42, ty + h1 + 2, 0, xr - x - 42, SUB, NULL);
	y += headh + 8;
	ui_hline(&G, x, y, inner, v->headRuleW, v->headRuleC, 0);
	y += v->headRuleW + 10;
	cairo_save(G.cr);
	cairo_rectangle(G.cr, s.x, y, s.w, s.y + s.h - 16 - noteh - y);
	cairo_clip(G.cr);
	for (int k = 0; k < count; k += cols) {
		double rh = sheet_row_h(&rows[first + k], colw, clamp, minh, &sf);
		if (cols == 2 && k + 1 < count) {
			double r2 = sheet_row_h(&rows[first + k + 1], colw, clamp, minh, &sf);
			rh = rh > r2 ? rh : r2;
		}
		paint_sheet_row(&rows[first + k], (struct rect){ x, y, colw, rh }, clamp, &sf);
		if (cols == 2 && k + 1 < count)
			paint_sheet_row(&rows[first + k + 1], (struct rect){ x + colw + 22, y, colw, rh }, clamp, &sf);
		y += rh;
	}
	cairo_restore(G.cr);
	y = s.y + s.h - 16 - noteh + 10;
	for (int k = 0; k < nnotes; k++)
		y += ui_text_wrap(&G, &sf, notes[k], x, y, inner, 6, 0, SUB) + 10;
}

/* ---- the scene ------------------------------------------------------------------------ */

void scene_paint(struct home *h, cairo_t *cr)
{
	H = h;
	G = (UiGc){ cr, 1, h->v };
	INK = h->v->ink;
	SUB = h->v->sub;
	PAPER = h->v->screenBg;
	h->nhits = 0;
	h->nregions = 0;
	ui_fill_rect(&G, 0, 0, h->W, h->H, PAPER);
	count_pages();
	double top = paint_header() + 12;
	const char *hint = h->edit ? "⤢ changes size · × removes · round buttons swap whole pages"
				   : "Tap an app to open it on a new screen · hold for more";
	UiFont hf = fpx(&h->v->dateFont, 12.5);
	double hh = text_h(&hf), hy = h->H - PAD_BOTTOM - hh;
	ui_text(&G, &hf, hint, h->W / 2.0, hy, 1, h->W - 2 * PAD_X, SUB, NULL);
	region_add((struct rect){ 0, hy - 2, h->W, h->H - hy + 2 }, R_HINT, -1); /* to the bottom edge: the glyphs reach below hh */
	paint_page(top, hy - 12);
	if (h->sheet != SHEET_NONE) {
		for (int i = 0; i < h->nhits; i++)
			h->hits[i].focus = false;
		paint_sheet();
	}
	/* the keyboard's ring: 2 px, 2 px out */
	if (h->focus >= 0) {
		int k = 0;
		for (int i = 0; i < h->nhits; i++)
			if (h->hits[i].focus && k++ == h->focus) {
				const struct rect *r = &h->hits[i].r;
				ui_stroke_rrect(&G, r->x - 4, r->y - 4, r->w + 8, r->h + 8, h->hits[i].radius + 4, 2, INK, 0);
				break;
			}
	}
}
