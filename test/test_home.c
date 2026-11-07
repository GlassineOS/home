// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The Home screen, offscreen: every home.* board of the design in the three voices,
 * portrait and landscape, as PNGs in out/renders/; taps and keys against fake launch and
 * hide commands (test/screen/launch.sh logs them); and what gets committed when a widget
 * runs again: nothing when its output is the same, one rectangle (its card) when it changed.
 *
 * Fixtures (test/screen/): desktop files (a Flatpak twin of KOReader among them), widgets
 * from apps and of the user's own, their data in a home directory, and Continue
 * reading's KOReader data from test/fixtures/reading. Pictures are drawn here.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

#include "../src/home.h"
#include "t.h"

static char top[1024], T[1100], HM[1200], LOG[1200];

/* ---- fixtures ------------------------------------------------------------------- */

static void sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sh(const char *fmt, ...)
{
	char cmd[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof cmd, fmt, ap);
	va_end(ap);
	if (system(cmd) != 0) {
		fprintf(stderr, "test_home: failed: %s\n", cmd);
		exit(1);
	}
}

static void put(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	if (!f || fputs(text, f) < 0 || fclose(f)) {
		fprintf(stderr, "test_home: can't write %s\n", path);
		exit(1);
	}
}

/* a picture to load: a moon, a meadow, a book cover */
static void picture(const char *path, int w, int h, int kind)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cairo_t *cr = cairo_create(s);
	if (kind == 0) { /* moon: a lit disc on black, 42 % */
		cairo_set_source_rgb(cr, 0.05, 0.05, 0.08);
		cairo_paint(cr);
		cairo_arc(cr, w / 2.0, h / 2.0, w * 0.42, 0, 2 * G_PI);
		cairo_set_source_rgb(cr, 0.9, 0.9, 0.85);
		cairo_fill(cr);
		cairo_arc(cr, w * 0.62, h / 2.0, w * 0.40, 0, 2 * G_PI);
		cairo_set_source_rgb(cr, 0.05, 0.05, 0.08);
		cairo_fill(cr);
	} else if (kind == 1) { /* meadow: sky gradient, river, grass */
		cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, 0, h);
		cairo_pattern_add_color_stop_rgb(p, 0, 0.55, 0.7, 0.9);
		cairo_pattern_add_color_stop_rgb(p, 0.55, 0.92, 0.94, 0.96);
		cairo_pattern_add_color_stop_rgb(p, 0.56, 0.35, 0.55, 0.25);
		cairo_pattern_add_color_stop_rgb(p, 1, 0.15, 0.3, 0.1);
		cairo_set_source(cr, p);
		cairo_paint(cr);
		cairo_pattern_destroy(p);
		cairo_move_to(cr, 0, h * 0.75);
		cairo_curve_to(cr, w * 0.3, h * 0.6, w * 0.6, h * 0.95, w, h * 0.7);
		cairo_set_line_width(cr, h * 0.06);
		cairo_set_source_rgb(cr, 0.7, 0.8, 0.9);
		cairo_stroke(cr);
	} else { /* cover: a dark cover with a light band and a lighthouse */
		cairo_set_source_rgb(cr, 0.12, 0.2, 0.3);
		cairo_paint(cr);
		cairo_rectangle(cr, 0, h * 0.1, w, h * 0.2);
		cairo_set_source_rgb(cr, 0.95, 0.9, 0.8);
		cairo_fill(cr);
		cairo_rectangle(cr, w * 0.45, h * 0.45, w * 0.1, h * 0.45);
		cairo_fill(cr);
		cairo_move_to(cr, w * 0.4, h * 0.45);
		cairo_line_to(cr, w * 0.5, h * 0.36);
		cairo_line_to(cr, w * 0.6, h * 0.45);
		cairo_fill(cr);
	}
	cairo_destroy(cr);
	cairo_surface_write_to_png(s, path);
	cairo_surface_destroy(s);
}

static const char CONF[] =
	"version = 1\n"
	"favourites = [\"koreader\", \"firefox-esr\", \"org.gnome.TextEditor\", \"foot\", \"org.gnome.Calendar\",\n"
	"  \"org.gnome.Nautilus\", \"glassine-settings\", \"org.gnome.Software\"]\n"
	"[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000001\"\nwidget = \"builtin/reading\"\nsize = \"wide\"\n"
	"[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000002\"\nwidget = \"system/notes-recent\"\nsize = \"half\"\n"
	"[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000003\"\nwidget = \"system/calendar-next\"\nsize = \"half\"\n"
	"[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000004\"\nwidget = \"home/favourites\"\nsize = 2\n"
	"[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000005\"\nwidget = \"system/package-updates\"\nsize = \"half\"\n"
	"[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000006\"\nwidget = \"user/sky\"\nsize = \"half\"\n"
	"[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000007\"\nwidget = \"home/all-apps\"\nsize = 3\n"
	"[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000008\"\nwidget = \"home/app-category\"\nsize = 2\n"
	"props = { category = \"Games\" }\n"
	"[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000009\"\nwidget = \"system/photo-frame\"\nsize = \"wide\"\n";

/* card indexes in CONF */
enum { C_READING, C_NOTES, C_CAL, C_FAVS, C_UPDATES, C_SKY, C_ALL, C_CAT, C_PHOTO };

static void setup(void)
{
	if (!getcwd(top, sizeof top))
		exit(1);
	snprintf(T, sizeof T, "%s/out/home-test", top);
	snprintf(HM, sizeof HM, "%s/home", T);
	snprintf(LOG, sizeof LOG, "%s/commands.log", T);
	sh("rm -rf '%s' && mkdir -p '%s' '%s/out/renders' && cp -R test/screen/home '%s' && cp -R test/fixtures/reading/home/. '%s'",
	   T, T, top, HM, HM);
	sh("find '%s' -name '*.lua' -exec sed -i 's|@HOME@|%s|g' {} +", HM, HM);
	/* the book was read two hours before the test's now */
	sh("sed -i 's|1699990000|1793988660|' '%s/.config/koreader/history.lua'", HM);
	sh("mkdir -p '%s/Pictures/Frame' '%s/.cache/sky' '%s/.cache/thumbnails/large' '%s/power/BAT0' '%s/.config'",
	   HM, HM, HM, T, HM);

	char p[2048];
	snprintf(p, sizeof p, "%s/.cache/sky/moon.png", HM);
	picture(p, 112, 112, 0);
	snprintf(p, sizeof p, "%s/Pictures/Frame/elbe.png", HM);
	picture(p, 480, 300, 1);
	/* KOReader's book has a freedesktop thumbnail, as a file manager would make it */
	snprintf(p, sizeof p, "%s/Books/The Lighthouse Keeper's Almanac.epub", HM);
	gchar *uri = g_filename_to_uri(p, NULL, NULL);
	gchar *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	snprintf(p, sizeof p, "%s/.cache/thumbnails/large/%s.png", HM, md5);
	picture(p, 170, 256, 2);
	g_free(uri);
	g_free(md5);

	snprintf(p, sizeof p, "%s/power/BAT0/type", T);
	put(p, "Battery\n");
	snprintf(p, sizeof p, "%s/power/BAT0/capacity", T);
	put(p, "69\n");
	snprintf(p, sizeof p, "%s/eink.conf", T);
	put(p, "@dither=off\n");

	char v[4096];
	setenv("HOME", HM, 1);
	setenv("TZ", "UTC", 1);
	tzset();
	snprintf(v, sizeof v, "%s/.config", HM);
	setenv("XDG_CONFIG_HOME", v, 1);
	snprintf(v, sizeof v, "%s/.cache", HM);
	setenv("XDG_CACHE_HOME", v, 1);
	snprintf(v, sizeof v, "%s/.local/state", HM);
	setenv("XDG_STATE_HOME", v, 1);
	snprintf(v, sizeof v, "%s/.local/share", HM);
	setenv("XDG_DATA_HOME", v, 1);
	snprintf(v, sizeof v, "%s/test/screen/share/applications:%s/test/screen/flatpak/exports/share/applications", top, top);
	setenv("GLASSINE_HOME_APPS", v, 1);
	snprintf(v, sizeof v, "%s/widgets", top);
	setenv("GLASSINE_HOME_WIDGETS", v, 1);
	snprintf(v, sizeof v, "%s/test/screen/system-widgets", top);
	setenv("GLASSINE_HOME_SYSTEM_WIDGETS", v, 1);
	snprintf(v, sizeof v, "%s/test/screen/user-widgets", top);
	setenv("GLASSINE_HOME_USER_WIDGETS", v, 1);
	snprintf(v, sizeof v, "%s/native", top);
	setenv("GLASSINE_HOME_NATIVE", v, 1);
	snprintf(v, sizeof v, "%s/home.conf", T);
	setenv("GLASSINE_HOME_CONF", v, 1);
	snprintf(v, sizeof v, "%s/eink.conf", T);
	setenv("GLASSINE_HOME_EINK_CONF", v, 1);
	snprintf(v, sizeof v, "%s/power", T);
	setenv("GLASSINE_HOME_POWER", v, 1);
	setenv("GLASSINE_HOME_NOW", "1793995860", 1); /* Friday 6 November 2026, 20:11 UTC */
	snprintf(v, sizeof v, "sh %s/test/screen/launch.sh launch", top);
	setenv("GLASSINE_HOME_LAUNCH", v, 1);
	snprintf(v, sizeof v, "sh %s/test/screen/launch.sh hide", top);
	setenv("GLASSINE_HOME_HIDE", v, 1);
	setenv("GLASSINE_HOME_TEST_LOG", LOG, 1);
}

/* a fresh layout file and no cached images: each Home starts from the same state */
static void reset(void)
{
	char p[2048];
	snprintf(p, sizeof p, "%s/home.conf", T);
	put(p, CONF);
	sh("rm -rf '%s/.cache/glassine-home' '%s/.local/state/glassine-home' '%s'", HM, HM, LOG);
}

/* ---- driving Home ----------------------------------------------------------------- */

static struct rect last_dmg[MAX_DAMAGE];
static int last_n;

static void on_commit(void *ud, const struct rect *r, int n)
{
	last_n = n;
	memcpy(last_dmg, r, (size_t)n * sizeof *r);
}

static long long ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static bool busy(const struct home *h, bool images)
{
	if (h->run)
		return true;
	for (size_t i = 0; i < h->ncards; i++) {
		const struct card *c = &h->cards[i];
		if (c->want_run || c->want_edit || c->db.pending)
			return true;
		for (int k = 0; images && k < DESC_MAX_IMAGES; k++)
			if (c->img[k].queued && !c->img[k].surf && !c->img[k].failed)
				return true;
	}
	return false;
}

/*
 * Runs the event loop: until nothing is left to do (for_ms 0), or for for_ms. Without
 * images, finished pictures stay in the queue (the images-pending board). watch_card:
 * set *ran when that card's widget ran.
 */
static void loop(struct home *h, bool images, int for_ms, int watch_card, bool *ran)
{
	long long end = ms() + (for_ms ? for_ms : 10000);
	while (ms() < end) {
		int wait = home_tick(h);
		if (ran && h->run && h->run_card == watch_card)
			*ran = true;
		if (!for_ms && !busy(h, images))
			break;
		int fds[8], n = home_fds(h, fds, 8);
		struct pollfd pf[8];
		int np = 0;
		for (int i = 0; i < n; i++)
			if (images || fds[i] != imgq_fd(h->iq))
				pf[np++] = (struct pollfd){ .fd = fds[i], .events = POLLIN };
		if (wait > 20)
			wait = 20;
		if (poll(pf, (nfds_t)np, wait) > 0)
			for (int i = 0; i < np; i++)
				if (pf[i].revents)
					home_ready(h, pf[i].fd);
	}
}

static int find_hit(const struct home *h, enum act act, int a, const char *id)
{
	for (int i = 0; i < h->nhits; i++)
		if (h->hits[i].act == act && (a < 0 || h->hits[i].a == a) && (!id || !strcmp(h->hits[i].id, id)))
			return i;
	return -1;
}

/* a point of hit i that no later hit covers (a page swap button sits on a peek's edge) */
static bool hit_point(const struct home *h, int i, double *px, double *py)
{
	const struct rect *r = &h->hits[i].r;
	for (int sy = 1; sy < 8; sy++)
		for (int sx = 1; sx < 8; sx++) {
			double x = r->x + r->w * sx / 8, y = r->y + r->h * sy / 8;
			int top = -1;
			for (int k = h->nhits - 1; k >= 0 && top < 0; k--) {
				const struct rect *q = &h->hits[k].r;
				if (x >= q->x && x < q->x + q->w && y >= q->y && y < q->y + q->h)
					top = k;
			}
			if (top == i) {
				*px = x;
				*py = y;
				return true;
			}
		}
	return false;
}

static bool tap_index(struct home *h, int i)
{
	double x, y;
	if (!hit_point(h, i, &x, &y)) {
		fprintf(stderr, "test_home: hit %d is covered\n", i);
		return false;
	}
	home_press(h, x, y);
	home_release(h, x, y);
	home_tick(h);
	return true;
}

static bool tap(struct home *h, enum act act, int a, const char *id)
{
	int i = find_hit(h, act, a, id);
	if (i < 0) {
		fprintf(stderr, "test_home: no hit for act %d (%d, %s)\n", act, a, id ? id : "-");
		return false;
	}
	return tap_index(h, i);
}

static bool hold(struct home *h, enum act act, int a, const char *id)
{
	int i = find_hit(h, act, a, id);
	if (i < 0)
		return false;
	double x = h->hits[i].r.x + h->hits[i].r.w / 2, y = h->hits[i].r.y + h->hits[i].r.h / 2;
	home_press(h, x, y);
	loop(h, true, HOLD_MS + 100, -1, NULL);
	home_release(h, x, y);
	home_tick(h);
	return h->sheet == SHEET_MENU;
}

/* paints and writes the board; every board but home.images waits for its pictures */
static void save(struct home *h, const char *voice, const char *orient, const char *state)
{
	home_paint(h, true);
	if (strcmp(state, "images")) {
		loop(h, true, 0, -1, NULL);
		home_paint(h, true);
	}
	char p[2048];
	snprintf(p, sizeof p, "%s/out/renders/home.%s-%s-%s.png", top, state, voice, orient);
	CHECK(cairo_surface_write_to_png(h->cur, p) == CAIRO_STATUS_SUCCESS);
}

/* the log's lines, waiting for want of them (the commands run detached) */
static int log_lines(char lines[][512], int want)
{
	int n = 0;
	for (long long end = ms() + 3000; ms() < end; usleep(20000)) {
		FILE *f = fopen(LOG, "r");
		n = 0;
		if (f) {
			while (n < 16 && fgets(lines[n], 512, f))
				lines[n][strcspn(lines[n], "\n")] = 0, n++;
			fclose(f);
		}
		if (n >= want)
			break;
	}
	return n;
}

static bool card_on_page(struct home *h, int card)
{
	for (int i = 0; i < h->nregions; i++)
		if (h->regions[i].kind == R_CARD && h->regions[i].card == card)
			return true;
	return false;
}

static void turn_to(struct home *h, int card)
{
	for (int p = 0; p < 6 && !card_on_page(h, card); p++) {
		h->page = p;
		h->dirty_now = true;
		home_paint(h, true);
	}
}


/* ---- Arrange and the layout file ------------------------------------------------ */

static void conf_path(char *p, size_t n)
{
	snprintf(p, n, "%s/home.conf", T);
}

/* the layout file as "widget:size …" and its uuids in order */
static void file_layout(char *out, size_t n, char uuids[][37], size_t *nu)
{
	struct home_conf c;
	char err[256], p[2048];
	conf_path(p, sizeof p);
	out[0] = 0;
	if (!conf_load(p, &c, err, sizeof err)) {
		snprintf(out, n, "unreadable: %s", err);
		return;
	}
	size_t l = 0;
	for (size_t i = 0; i < c.n; i++) {
		const char *w = c.w[i].widget;
		char sz[8];
		if (c.w[i].size == SIZE_HALF)
			snprintf(sz, sizeof sz, "h");
		else if (c.w[i].size == SIZE_WIDE)
			snprintf(sz, sizeof sz, "w");
		else
			snprintf(sz, sizeof sz, "%d", c.w[i].size);
		l += (size_t)snprintf(out + l, n - l, "%s%s:%s", i ? " " : "", strchr(w, '/') + 1, sz);
		if (uuids && i < 32)
			memcpy(uuids[i], c.w[i].uuid, 37);
	}
	if (nu)
		*nu = c.n;
	conf_free(&c);
}

/* "00…0N" for CONF's widgets, "new" for one Home made */
static void uuid_tags(char *out, size_t n)
{
	char u[32][37], lay[2048];
	size_t nu = 0, l = 0;
	file_layout(lay, sizeof lay, u, &nu);
	out[0] = 0;
	for (size_t i = 0; i < nu && i < 32; i++) {
		bool fixed = !strncmp(u[i], "00000000-0000-4000-8000-00000000000", 35);
		if (fixed)
			l += (size_t)snprintf(out + l, n - l, "%s%c", i ? " " : "", u[i][35]);
		else
			l += (size_t)snprintf(out + l, n - l, "%snew", i ? " " : "");
	}
}

/*
 * The last commit's rectangles, each named by the region it is: the smallest region
 * whose rectangle it covers, grown by at most a tile of changed pixels beyond it
 * ("card3", "page", "header", "sheet"); "all" for the whole canvas.
 */
static void damage_names(const struct home *h, char *out, size_t n)
{
	char names[MAX_DAMAGE][24];
	int k = 0;
	for (int i = 0; i < last_n; i++) {
		const struct rect *d = &last_dmg[i];
		int best = -1;
		double area = 1e18;
		if (d->x <= 0 && d->y <= 0 && d->w >= h->W * h->S && d->h >= h->H * h->S) {
			snprintf(names[k++], 24, "all");
			continue;
		}
		for (int r = 0; r < h->nregions; r++) {
			struct rect g = h->regions[r].r;
			double x0 = g.x * h->S, y0 = g.y * h->S, x1 = (g.x + g.w) * h->S, y1 = (g.y + g.h) * h->S;
			bool covers = d->x <= x0 + 1 && d->y <= y0 + 1 && d->x + d->w >= x1 - 1 && d->y + d->h >= y1 - 1;
			bool near = d->x >= x0 - 17 && d->y >= y0 - 17 && d->x + d->w <= x1 + 17 && d->y + d->h <= y1 + 17;
			if (covers && near && g.w * g.h < area) {
				area = g.w * g.h;
				best = r;
			}
		}
		if (best < 0) {
			snprintf(names[k++], 24, "?%.0f,%.0f,%.0fx%.0f", d->x, d->y, d->w, d->h);
			continue;
		}
		const struct region *g = &h->regions[best];
		static const char *const kinds[] = { "header", "page", "card", "slot", "hint", "sheet" };
		if (g->kind == R_CARD || g->kind == R_SLOT)
			snprintf(names[k++], 24, "%s%d", kinds[g->kind], g->card);
		else
			snprintf(names[k++], 24, "%s", kinds[g->kind]);
	}
	/* sorted, so a check doesn't depend on the scan order */
	for (int i = 0; i < k; i++)
		for (int j = i + 1; j < k; j++)
			if (strcmp(names[j], names[i]) < 0) {
				char t[24];
				memcpy(t, names[i], 24);
				memcpy(names[i], names[j], 24);
				memcpy(names[j], t, 24);
			}
	size_t l = 0;
	out[0] = 0;
	for (int i = 0; i < k; i++)
		l += (size_t)snprintf(out + l, n - l, "%s%s", i ? " " : "", names[i]);
}

/* A tap that must be one action: one commit, its damage named; then the event loop runs dry. */
static int commits_before;

/* runs until nothing is left to paint: widget runs, images (requested when a card is laid out) */
static void settle(struct home *h)
{
	for (int i = 0; i < 6; i++) {
		loop(h, true, 0, -1, NULL);
		int c = h->commits;
		home_paint(h, true);
		if (c == h->commits && !busy(h, true))
			break;
	}
}

static bool act(struct home *h, enum act a, int arg, char *dmg, size_t n)
{
	settle(h);
	commits_before = h->commits;
	last_n = 0;
	bool ok = tap(h, a, arg, NULL);
	CHECK_INT(h->commits - commits_before, 1);
	damage_names(h, dmg, n);
	return ok;
}

static int hit_count(const struct home *h, enum act a)
{
	int n = 0;
	for (int i = 0; i < h->nhits; i++)
		n += h->hits[i].act == a;
	return n;
}

static void show_page(struct home *h, int page)
{
	h->page = page;
	h->dirty_now = true;
	home_paint(h, true);
}

/* turns sheet pages until a gallery row's Add shows; false if it never does */
static bool gallery_find(struct home *h, int row)
{
	for (int p = 0; p < 8; p++) {
		if (find_hit(h, A_GAL_ADD, row, NULL) >= 0)
			return true;
		if (find_hit(h, A_SHEET_NEXT, -1, NULL) < 0 || !tap(h, A_SHEET_NEXT, -1, NULL))
			return false;
	}
	return false;
}

static int gallery_row(const struct home *h, const char *ref)
{
	for (size_t e = 0; e < h->cat.n; e++)
		if (!strcmp(h->cat.e[e].ref, ref))
			return 3 + (int)e;
	return -1;
}

static int swaps_refused; /* shared edges where a swap would move a widget to another page */

/* every offered swap is two widgets on this page sharing an edge that pack_can_swap allows, and every such pair is offered */
static void check_swaps_offered(struct home *h)
{
	size_t n = h->conf.n;
	int *sizes = calloc(n + 1, sizeof *sizes);
	struct pack_pos *pos = calloc(n + 1, sizeof *pos);
	for (size_t i = 0; i < n; i++)
		sizes[i] = h->conf.w[i].size;
	pack(h->orient, sizes, n, pos);
	int want = 0;
	for (size_t a = 0; a < n; a++)
		for (size_t b = 0; b < n; b++) {
			if (a == b || pos[a].page != h->page || pos[b].page != h->page)
				continue;
			int ax0 = pos[a].lane * 4 + pos[a].col, ax1 = ax0 + pos[a].cols, ay0 = pos[a].row, ay1 = ay0 + pos[a].rows;
			int bx0 = pos[b].lane * 4 + pos[b].col, bx1 = bx0 + pos[b].cols, by0 = pos[b].row, by1 = by0 + pos[b].rows;
			int oy0 = ay0 > by0 ? ay0 : by0, oy1 = ay1 < by1 ? ay1 : by1;
			int ox0 = ax0 > bx0 ? ax0 : bx0, ox1 = ax1 < bx1 ? ax1 : bx1;
			bool edge = (ax1 == bx0 && oy0 < oy1 ) || (ay1 == by0 && ox0 < ox1);
			bool can = pack_can_swap(h->orient, sizes, n, a, b);
			bool shown = false;
			for (int i = 0; i < h->nhits; i++)
				shown |= h->hits[i].act == A_SWAP && h->hits[i].a == (int)a && h->hits[i].b == (int)b;
			if (shown != (edge && can))
				fprintf(stderr, "test_home: swap %zu-%zu: shown %d, edge %d, can %d\n", a, b, shown, edge, can);
			CHECK(shown == (edge && can));
			want += edge && can;
			swaps_refused += edge && !can;
		}
	CHECK_INT(hit_count(h, A_SWAP), want);
	free(sizes);
	free(pos);
}

/* ---- the boards ----------------------------------------------------------------- */

static void boards(struct zygote *z, const char *voice, bool landscape)
{
	const char *o = landscape ? "landscape" : "portrait";
	setenv("GLASSINE_HOME_VOICE", voice, 1);
	reset();
	struct home h;
	struct home_io io = { .commit = on_commit };
	home_init(&h, z, &io);
	ui_draw_init();
	home_resize(&h, landscape ? 936 : 702, landscape ? 702 : 884, 1);
	home_appear(&h);

	/* home.images: laid out at once, frames until the pictures are ready */
	loop(&h, false, 0, -1, NULL);
	save(&h, voice, o, "images");
	CHECK(h.cards[C_READING].img[0].queued && !h.cards[C_READING].img[0].surf);
	CHECK(h.cards[C_SKY].img[0].queued || !card_on_page(&h, C_SKY));
	for (size_t i = 0; i < h.ncards; i++)
		if (h.cards[i].m && h.cards[i].nat == NAT_NONE && h.cards[i].st.last != RUN_OK)
			fprintf(stderr, "test_home: card %zu: %s\n", i, h.cards[i].fail);

	loop(&h, true, 0, -1, NULL);
	home_paint(&h, true);
	loop(&h, true, 0, -1, NULL);
	save(&h, voice, o, "screen");
	CHECK(h.cards[C_READING].img[0].surf != NULL);

	turn_to(&h, C_ALL);
	save(&h, voice, o, "page-2");
	turn_to(&h, C_PHOTO);
	loop(&h, true, 0, -1, NULL);
	home_paint(&h, true);
	loop(&h, true, 0, -1, NULL);
	save(&h, voice, o, "page-3");
	CHECK(h.cards[C_PHOTO].img[0].surf != NULL);

	h.page = 0;
	h.dirty_now = true;
	home_paint(&h, true);
	CHECK(hold(&h, A_APP, -1, "koreader"));
	save(&h, voice, o, "app-menu");
	home_key(&h, K_ESCAPE);
	home_tick(&h);
	CHECK(h.sheet == SHEET_NONE);

	CHECK(tap(&h, A_ARRANGE, -1, NULL));
	CHECK(h.edit);
	save(&h, voice, o, "edit");
	/* a focus ring, on the first focusable control */
	home_key(&h, K_TAB);
	home_tick(&h);
	save(&h, voice, o, "edit-focus");
	home_key(&h, K_TAB);
	home_key(&h, K_BACKTAB);
	home_key(&h, K_BACKTAB);
	home_tick(&h);

	turn_to(&h, C_CAT);
	CHECK(tap(&h, A_CTL_SETTINGS, C_CAT, NULL));
	CHECK(h.sheet == SHEET_SETTINGS);
	loop(&h, true, 0, -1, NULL);
	save(&h, voice, o, "widget-settings");
	home_key(&h, K_ESCAPE);
	home_tick(&h);

	/* the Add widget card sits after the last widget (in landscape on a page of its own) */
	for (int p = 0; p < 6 && find_hit(&h, A_ADD_CARD, -1, NULL) < 0; p++) {
		h.page = p;
		h.dirty_now = true;
		home_paint(&h, true);
	}
	CHECK(tap(&h, A_ADD_CARD, -1, NULL));
	CHECK(h.sheet == SHEET_GALLERY);
	save(&h, voice, o, "add-widget");
	home_key(&h, K_ESCAPE);
	home_key(&h, K_ESCAPE);
	home_tick(&h);
	CHECK(!h.edit);

	home_free(&h);
}

/* ---- taps and keys -------------------------------------------------------------- */

static void commands(struct zygote *z)
{
	setenv("GLASSINE_HOME_VOICE", "white", 1);
	reset();
	struct home h;
	struct home_io io = { .commit = on_commit };
	home_init(&h, z, &io);
	ui_draw_init();
	home_resize(&h, 702, 884, 1);
	home_appear(&h);
	loop(&h, true, 0, -1, NULL);
	home_paint(&h, true);

	char l[16][512];
	/* a tap opens on a new screen */
	CHECK(tap(&h, A_APP, -1, "firefox-esr"));
	CHECK_INT(log_lines(l, 1), 1);
	CHECK_STR(l[0], "launch firefox-esr new");

	/* hold, then Open on this screen */
	CHECK(hold(&h, A_APP, -1, "org.gnome.Calendar"));
	CHECK(tap(&h, A_MENU_HERE, -1, NULL));
	CHECK_INT(log_lines(l, 2), 2);
	CHECK_STR(l[1], "launch org.gnome.Calendar here");
	CHECK(h.sheet == SHEET_NONE);

	/* Continue reading's button: KOReader with the book; "koreader" resolves to the
	 * package (exact id) though the Flatpak exists too */
	int b = find_hit(&h, A_WBUTTON, C_READING, NULL);
	CHECK(b >= 0);
	CHECK(tap(&h, A_WBUTTON, C_READING, NULL));
	CHECK_INT(log_lines(l, 3), 3);
	char want[1024];
	snprintf(want, sizeof want, "launch koreader new %s/Books/The Lighthouse Keeper's Almanac.epub", HM);
	CHECK_STR(l[2], want);

	/* Updates' Review: a Settings page */
	CHECK(tap(&h, A_WBUTTON, C_UPDATES, NULL));
	CHECK_INT(log_lines(l, 4), 4);
	CHECK_STR(l[3], "launch glassine-settings new updates");

	/* keys: Tab to the first app of Favourite apps and Enter; Escape hides */
	int n = 0;
	for (int i = 0; i < 64; i++) {
		home_key(&h, K_TAB);
		home_tick(&h);
		int fi = 0, k = -1;
		for (int j = 0; j < h.nhits; j++)
			if (h.hits[j].focus && fi++ == h.focus)
				k = j;
		if (k >= 0 && h.hits[k].act == A_APP) {
			n = 1;
			home_key(&h, K_ENTER);
			home_tick(&h);
			CHECK_INT(log_lines(l, 5), 5);
			char w2[600];
			snprintf(w2, sizeof w2, "launch %s new", h.hits[k].id);
			CHECK_STR(l[4], w2);
			break;
		}
	}
	CHECK_INT(n, 1);
	home_key(&h, K_ESCAPE);
	CHECK_INT(log_lines(l, 6), 6);
	CHECK_STR(l[5], "hide");

	/* a removed favourite (the menu's toggle) is saved in the layout file */
	CHECK(hold(&h, A_APP, -1, "foot"));
	CHECK(tap(&h, A_MENU_FAV, -1, NULL));
	CHECK(!home_is_fav(&h, "foot"));
	struct home_conf c;
	char err[256], p[2048];
	snprintf(p, sizeof p, "%s/home.conf", T);
	CHECK(conf_load(p, &c, err, sizeof err));
	bool has = false;
	for (size_t i = 0; i < c.nfav; i++)
		has |= !strcmp(c.favourites[i], "foot");
	CHECK(!has);
	conf_free(&c);

	/* ---- commits: an unchanged run commits nothing; a changed card is one rectangle */
	h.sheet = SHEET_NONE;
	h.dirty_now = true;
	loop(&h, true, 1500, -1, NULL);
	int before = h.commits;
	char np[2048];
	snprintf(np, sizeof np, "%s/data/notes.lua", HM);
	sh("cp '%s' '%s.new' && mv '%s.new' '%s'", np, np, np, np); /* same bytes, a new file */
	bool ran = false;
	loop(&h, true, 2500, C_NOTES, &ran);
	CHECK(ran);
	CHECK_INT(h.commits, before);

	snprintf(np, sizeof np, "%s/data/updates.lua", HM);
	put(np, "return { names = { \"Firefox ESR\", \"KOReader\", \"libwayland\", \"mesa\" }, checked = \"07:12\" }\n");
	ran = false;
	loop(&h, true, 2500, C_UPDATES, &ran);
	CHECK(ran);
	CHECK_INT(h.commits, before + 1);
	CHECK_INT(last_n, 1);
	struct rect cr = { 0 };
	for (int i = 0; i < h.nregions; i++)
		if (h.regions[i].kind == R_CARD && h.regions[i].card == C_UPDATES)
			cr = h.regions[i].r;
	CHECK(cr.w > 0);
	CHECK(last_dmg[0].x >= cr.x - 1 && last_dmg[0].y >= cr.y - 1 &&
	      last_dmg[0].x + last_dmg[0].w <= cr.x + cr.w + 1 && last_dmg[0].y + last_dmg[0].h <= cr.y + cr.h + 1);

	home_free(&h);
}

/* ---- a run in flight while the layout or the catalog changes ------------------------ */

/* starts card's widget run on the next tick; the run is in flight until Home steps it */
static bool start_run(struct home *h, int card)
{
	h->cards[card].want_run = true;
	h->cards[card].trig = TRIGGER_APPEAR;
	home_tick(h);
	return h->run && h->run_card == card;
}

/* every card's last output, by instance */
struct outputs {
	size_t n;
	char uuid[16][37];
	char *json[16];
};

static void outputs_of(const struct home *h, struct outputs *o)
{
	o->n = 0;
	for (size_t i = 0; i < h->ncards && i < 16; i++, o->n++) {
		snprintf(o->uuid[i], sizeof o->uuid[i], "%s", h->cards[i].uuid);
		struct buf j = { 0 };
		if (h->cards[i].desc)
			v_to_json(h->cards[i].desc, &j);
		o->json[i] = strdup(j.p ? j.p : "");
		buf_free(&j);
	}
}

/* each card still in h shows what it showed in before */
static void same_outputs(const struct home *h, const struct outputs *before)
{
	struct outputs now;
	outputs_of(h, &now);
	for (size_t i = 0; i < now.n; i++)
		for (size_t k = 0; k < before->n; k++)
			if (!strcmp(now.uuid[i], before->uuid[k]))
				CHECK_STR(now.json[i], before->json[k]);
	for (size_t i = 0; i < now.n; i++)
		free(now.json[i]);
}

static void outputs_free(struct outputs *o)
{
	for (size_t i = 0; i < o->n; i++)
		free(o->json[i]);
	o->n = 0;
}

static void in_flight(struct zygote *z)
{
	setenv("GLASSINE_HOME_VOICE", "white", 1);
	reset();
	/* a user widget root of the test's own, to add a folder to */
	char users[2048], was[2048];
	snprintf(was, sizeof was, "%s", getenv("GLASSINE_HOME_USER_WIDGETS"));
	snprintf(users, sizeof users, "%s/user-widgets", T);
	sh("rm -rf '%s' && cp -R test/screen/user-widgets '%s'", users, users);
	setenv("GLASSINE_HOME_USER_WIDGETS", users, 1);
	struct home h;
	struct home_io io = { .commit = on_commit };
	home_init(&h, z, &io);
	home_resize(&h, 702, 884, 1);
	home_appear(&h);
	loop(&h, true, 0, -1, NULL);
	CHECK_INT(h.cards[C_PHOTO].st.last, RUN_OK);

	/* a widget folder appears while Photo of the day runs: the catalog is rescanned
	 * (its manifests freed) before the run's file request and image are checked */
	CHECK(start_run(&h, C_PHOTO));
	sh("mkdir '%s/arrived'", users);
	struct pollfd pf = { .fd = h.ino, .events = POLLIN };
	CHECK(poll(&pf, 1, 2000) == 1);
	home_ready(&h, h.ino);
	CHECK(h.run != NULL); /* still in flight */
	loop(&h, true, 0, -1, NULL);
	CHECK_INT(h.cards[C_PHOTO].st.last, RUN_OK);
	CHECK_STR(h.cards[C_PHOTO].fail, "");
	struct buf j = { 0 };
	v_to_json(h.cards[C_PHOTO].desc, &j);
	CHECK(j.p && strstr(j.p, "Pictures/Frame/elbe.png") != NULL);
	buf_free(&j);

	/* Arrange swaps Notes and Calendar while Notes runs: the result goes to Notes,
	 * now at Calendar's index, and Calendar keeps its own */
	CHECK(tap(&h, A_ARRANGE, -1, NULL));
	struct outputs before;
	outputs_of(&h, &before);
	CHECK(strcmp(before.json[C_NOTES], before.json[C_CAL]) != 0);
	CHECK(start_run(&h, C_NOTES));
	CHECK(tap(&h, A_SWAP, C_NOTES, NULL));
	CHECK_STR(h.cards[C_CAL].uuid, before.uuid[C_NOTES]); /* swapped */
	CHECK_INT(h.run_card, C_CAL);
	loop(&h, true, 0, -1, NULL);
	CHECK_INT(h.cards[C_CAL].st.last, RUN_OK);
	same_outputs(&h, &before);

	/* Notes (now at index C_CAL) is removed while it runs: its result goes nowhere,
	 * and the card that takes its index keeps its own output */
	home_paint(&h, true);
	CHECK(start_run(&h, C_CAL));
	CHECK(tap(&h, A_CTL_REMOVE, C_CAL, NULL));
	CHECK_INT(h.ncards, before.n - 1);
	CHECK_INT(h.run_card, -1);
	loop(&h, true, 0, -1, NULL);
	same_outputs(&h, &before);
	outputs_free(&before);

	home_free(&h);
	setenv("GLASSINE_HOME_USER_WIDGETS", was, 1);
}

/* ---- touch targets: a press counts within 48 px around a smaller control (FR-041) ---- */

static void press_at(struct home *h, double x, double y)
{
	home_press(h, x, y);
	home_release(h, x, y);
	home_tick(h);
}

static struct rect hit_rect(const struct home *h, enum act act, int a)
{
	int i = find_hit(h, act, a, NULL);
	CHECK(i >= 0);
	return i >= 0 ? h->hits[i].r : (struct rect){ -100, -100, 0, 0 };
}

static void touch_targets(struct zygote *z)
{
	setenv("GLASSINE_HOME_VOICE", "white", 1);
	reset();
	struct home h;
	struct home_io io = { .commit = on_commit };
	home_init(&h, z, &io);
	home_resize(&h, 702, 884, 1);
	home_appear(&h);
	loop(&h, true, 0, -1, NULL);
	home_paint(&h, true);

	/* the header's next page: 44 px drawn, pressed 1 px below it */
	struct rect r = hit_rect(&h, A_PAGE_NEXT, -1);
	CHECK(r.h < TOUCH_MIN);
	int page = h.page;
	press_at(&h, r.x + r.w / 2, r.y + r.h + 1);
	CHECK_INT(h.page, page + 1);
	h.page = 0;
	h.dirty_now = true;
	home_tick(&h);

	/* Arrange's card controls: 40 px, 6 px apart. A press in the gap goes to the
	 * nearer one; one just above the remove button removes */
	CHECK(tap(&h, A_ARRANGE, -1, NULL));
	struct rect rm = hit_rect(&h, A_CTL_REMOVE, C_FAVS), sz = hit_rect(&h, A_CTL_SIZE, C_FAVS);
	CHECK(rm.w < TOUCH_MIN && rm.x - (sz.x + sz.w) == 6);
	int size = h.conf.w[C_FAVS].size;
	size_t n = h.ncards;
	press_at(&h, sz.x + sz.w + 2, sz.y + sz.h / 2); /* 2 px from size, 4 from remove */
	CHECK(h.conf.w[C_FAVS].size != size);
	CHECK_INT(h.ncards, n);
	rm = hit_rect(&h, A_CTL_REMOVE, C_CAL);
	press_at(&h, rm.x + rm.w / 2, rm.y - 3);
	CHECK_INT(h.ncards, n - 1);

	/* an open sheet's backdrop is a barrier: a press at a control under it (drawn box or
	 * its touch area) closes the sheet and does nothing else */
	for (int p = 0; p < 6 && find_hit(&h, A_ADD_CARD, -1, NULL) < 0; p++) {
		h.page = p;
		h.dirty_now = true;
		home_paint(&h, true);
	}
	CHECK(tap(&h, A_ADD_CARD, -1, NULL));
	CHECK(h.sheet == SHEET_GALLERY);
	n = h.ncards;
	int barrier = -1;
	for (int i = 0; i < h.nhits; i++)
		if (h.hits[i].act == A_SHEET_OUTSIDE)
			barrier = i;
	CHECK(barrier >= 0);
	/* a control under the sheet, and a point in its touch area clear of the sheet's own */
	double px = -1, py = -1;
	for (int i = 0; i < barrier && px < 0; i++) {
		struct rect u = h.hits[i].r;
		if (u.w >= TOUCH_MIN)
			continue;
		double x = u.x - (TOUCH_MIN - u.w) / 2 + 1, y = u.y + u.h / 2;
		bool clear = true;
		for (int k = barrier + 1; k < h.nhits && clear; k++) {
			struct rect t = h.hits[k].r;
			double gw = t.w < TOUCH_MIN ? (TOUCH_MIN - t.w) / 2 : 0, gh = t.h < TOUCH_MIN ? (TOUCH_MIN - t.h) / 2 : 0;
			clear = !(x >= t.x - gw && x < t.x + t.w + gw && y >= t.y - gh && y < t.y + t.h + gh);
		}
		if (clear)
			px = x, py = y;
	}
	CHECK(px >= 0);
	press_at(&h, px, py);
	CHECK(h.sheet == SHEET_NONE);
	CHECK_INT(h.ncards, n);

	home_free(&h);
}

/* ---- wl.c's frames: a paint while both buffers are busy is owed, and nothing is lost --- */

/* the widget helper is killed while Home is idle: the next run fails at once with the
 * message, and home_helper_gone tells the loop to exit (wl.c) */
static void helper_gone(struct zygote *z)
{
	setenv("GLASSINE_HOME_VOICE", "white", 1);
	reset();
	struct home h;
	struct home_io io = { .commit = on_commit };
	home_init(&h, z, &io);
	home_resize(&h, 702, 884, 1);
	home_appear(&h);
	loop(&h, true, 0, -1, NULL);
	CHECK_INT(h.cards[C_NOTES].st.last, RUN_OK);
	CHECK(!home_helper_gone(&h));
	kill(z->pid, SIGKILL);
	waitpid(z->pid, NULL, 0);
	start_run(&h, C_NOTES);
	loop(&h, true, 0, -1, NULL);
	CHECK(home_helper_gone(&h));
	CHECK_INT(h.cards[C_NOTES].st.last, RUN_CRASH);
	CHECK_STR(h.cards[C_NOTES].fail, "could not start the run: Home's widget helper stopped; widgets run again when Home restarts");
	home_free(&h);
}

static void frames_busy(void)
{
	struct frames f;
	frames_init(&f);
	struct rect a = { 0, 0, 10, 10 }, b = { 20, 20, 5, 5 }, c = { 40, 0, 8, 8 }, d = { 0, 40, 6, 6 };
	frames_damage(&f, &a, 1);
	CHECK_INT(frames_pick(&f), 0);
	CHECK(f.b[0].all); /* never shown: copied whole */
	frames_shown(&f, 0);
	frames_damage(&f, &b, 1);
	CHECK_INT(frames_pick(&f), 1);
	frames_shown(&f, 1);
	CHECK(!f.owed);

	/* both with the compositor: two paints wait, nothing is committed */
	frames_damage(&f, &c, 1);
	CHECK_INT(frames_pick(&f), -1);
	CHECK(f.owed);
	frames_damage(&f, &d, 1);
	CHECK_INT(frames_pick(&f), -1);

	/* buffer 0 comes back: it copies all it missed (b, c, d), the surface damages
	 * what changed since the last commit (c, d), and nothing is owed any more */
	frames_released(&f, 0);
	CHECK_INT(frames_pick(&f), 0);
	CHECK(!f.b[0].all);
	CHECK_INT(f.b[0].ndmg, 3);
	CHECK(f.b[0].ndmg == 3 && f.b[0].dmg[0].x == 20 && f.b[0].dmg[1].x == 40 && f.b[0].dmg[2].y == 40);
	CHECK_INT(f.nsdmg, 2);
	CHECK(f.nsdmg == 2 && f.sdmg[0].x == 40 && f.sdmg[1].y == 40);
	frames_shown(&f, 0);
	CHECK(!f.owed);
	CHECK_INT(f.nsdmg, 0);
	/* buffer 1, still away, keeps c and d for when it is used next */
	CHECK_INT(f.b[1].ndmg, 2);

	/* more rectangles than the lists hold: the buffer and the surface go whole */
	frames_released(&f, 1);
	struct rect many[MAX_DAMAGE + 1];
	for (int i = 0; i <= MAX_DAMAGE; i++)
		many[i] = (struct rect){ i, 0, 1, 1 };
	frames_damage(&f, many, MAX_DAMAGE + 1);
	CHECK(f.b[1].all && f.sall);
	CHECK_INT(frames_pick(&f), 1);
	frames_shown(&f, 1);
	CHECK(!f.b[1].all && !f.sall);

	/* the re-review's sequence: buffer 0 is held while the canvas turns 70 x 88 -> 88 x 70,
	 * paints twice there and turns back; buffer 0 is the old size again but copies whole,
	 * not a rect from the wide canvas */
	frames_init(&f);
	frames_canvas(&f, 70, 88);
	CHECK_INT(frames_pick(&f), 0);
	frames_shown(&f, 0);
	frames_canvas(&f, 88, 70);
	CHECK(f.b[0].all && f.b[1].all && f.sall);
	struct rect wide = { 80, 0, 8, 8 };
	frames_damage(&f, &wide, 1);
	CHECK_INT(frames_pick(&f), 1);
	frames_shown(&f, 1);
	frames_released(&f, 1);
	frames_damage(&f, &wide, 1);
	CHECK_INT(frames_pick(&f), 1);
	frames_shown(&f, 1);
	frames_canvas(&f, 70, 88);
	frames_released(&f, 0);
	CHECK_INT(frames_pick(&f), 0);
	CHECK(f.b[0].all && f.b[0].ndmg == 0 && f.sall);
	frames_canvas(&f, 70, 88); /* the same size: nothing changes */
	frames_shown(&f, 0);
	frames_canvas(&f, 70, 88);
	CHECK(!f.b[0].all && !f.sall);

	/* a rect is clipped to the buffer; one outside it or empty is skipped */
	int x0, y0, x1, y1;
	CHECK(!frames_clip(&wide, 70, 88, &x0, &y0, &x1, &y1));
	CHECK(frames_clip(&(struct rect){ -5, -5, 10, 10 }, 70, 88, &x0, &y0, &x1, &y1));
	CHECK(x0 == 0 && y0 == 0 && x1 == 5 && y1 == 5);
	CHECK(frames_clip(&(struct rect){ 60, 80, 20, 20 }, 70, 88, &x0, &y0, &x1, &y1));
	CHECK(x0 == 60 && y0 == 80 && x1 == 70 && y1 == 88);
	CHECK(!frames_clip(&(struct rect){ 10, 10, 0, 5 }, 70, 88, &x0, &y0, &x1, &y1));
	CHECK(!frames_clip(&(struct rect){ 10, 90, 5, 5 }, 70, 88, &x0, &y0, &x1, &y1));
	CHECK(!frames_clip(&(struct rect){ NAN, 0, 5, 5 }, 70, 88, &x0, &y0, &x1, &y1));
}

/* ---- image limits: a source's pixels, not its bytes, decide what decoding costs ------- */

static long peak_rss_kb(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_maxrss;
}

static unsigned char *prepare_file(const char *path, int w, int h, bool contain, long long max_px)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(fd >= 0);
	unsigned char *g = fd >= 0 ? img_prepare(fd, w, h, contain, 0, max_px) : NULL;
	if (fd >= 0)
		close(fd);
	return g;
}

/* ---- pictures by the byte: headers to measure, and a large PNG without a compressor ---- */

static uint32_t crc32_of(uint32_t c, const unsigned char *p, size_t n)
{
	c = ~c;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = c >> 1 ^ (0xedb88320u & -(c & 1));
	}
	return ~c;
}

static void be32_put(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void le32_put(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

/* a PNG chunk at p: its length, type, data and CRC; returns its size */
static size_t png_chunk(unsigned char *p, const char *type, const void *d, uint32_t n)
{
	be32_put(p, n);
	memcpy(p + 4, type, 4);
	if (n)
		memcpy(p + 8, d, n);
	be32_put(p + 8 + n, crc32_of(0, p + 4, (size_t)n + 4));
	return (size_t)n + 12;
}

/* the signature and IHDR of a w x h 8-bit grey PNG */
static size_t png_start(unsigned char *p, uint32_t w, uint32_t h)
{
	unsigned char ih[13] = { 0 };
	be32_put(ih, w);
	be32_put(ih + 4, h);
	ih[8] = 8;
	memcpy(p, "\x89PNG\r\n\x1a\n", 8);
	return 8 + png_chunk(p + 8, "IHDR", ih, 13);
}

/* deflate's bits, the lowest first; a Huffman code goes in from its highest bit */
struct bits { unsigned char *p; size_t n; unsigned acc, nb; };

static void bits_put(struct bits *b, uint32_t v, int n)
{
	for (int i = 0; i < n; i++) {
		b->acc |= (v >> i & 1) << b->nb;
		if (++b->nb == 8) {
			b->p[b->n++] = (unsigned char)b->acc;
			b->acc = b->nb = 0;
		}
	}
}

static void bits_code(struct bits *b, uint32_t code, int n)
{
	for (int i = n - 1; i >= 0; i--)
		bits_put(b, code >> i & 1, 1);
}

/*
 * A valid w x h 8-bit grey PNG, all black, without a compressor: every byte of the image
 * data is 0 (each row's filter, then its pixels), so it is one fixed-Huffman deflate block
 * of a literal 0 and copies of 258 bytes at distance 1. 16000 x 16000 is 1.6 MB.
 */
static void grey_png(const char *path, uint32_t w, uint32_t h)
{
	unsigned long long total = ((unsigned long long)w + 1) * h, left = total - 1;
	size_t cap = (size_t)(total / 258 * 13 / 8) + 512;
	unsigned char *z = malloc(cap), *png = malloc(cap + 128);
	CHECK(z && png);
	if (!z || !png)
		exit(1);
	struct bits b = { z, 0, 0, 0 };
	z[b.n++] = 0x78; /* zlib: deflate, 32K window */
	z[b.n++] = 0x01;
	bits_put(&b, 1, 1);     /* the last block */
	bits_put(&b, 1, 2);     /* fixed Huffman codes */
	bits_code(&b, 0x30, 8); /* literal 0 */
	for (; left >= 258; left -= 258) {
		bits_code(&b, 0xc5, 8); /* length 258 (code 285) */
		bits_code(&b, 0, 5);    /* distance 1 */
	}
	for (; left; left--)
		bits_code(&b, 0x30, 8);
	bits_code(&b, 0, 7); /* end of block */
	if (b.nb)
		bits_put(&b, 0, 8 - (int)b.nb);
	be32_put(z + b.n, (uint32_t)(total % 65521) << 16 | 1); /* Adler-32 of zeros */
	b.n += 4;
	size_t n = png_start(png, w, h);
	n += png_chunk(png + n, "IDAT", z, (uint32_t)b.n);
	n += png_chunk(png + n, "IEND", NULL, 0);
	FILE *f = fopen(path, "wb");
	CHECK(f && fwrite(png, 1, n, f) == n && fclose(f) == 0);
	free(z);
	free(png);
}

static bool measures(const void *d, size_t n, const char *type, int w, int h)
{
	int mw = -1, mh = -1;
	const char *t = img_measure(d, n, &mw, &mh);
	if (!type)
		return t == NULL;
	if (!t || strcmp(t, type) || mw != w || mh != h) {
		fprintf(stderr, "test_home: measured %s %d x %d, want %s %d x %d\n", t ? t : "(none)", mw, mh, type, w, h);
		return false;
	}
	return true;
}

/* the header parsers on crafted bytes: what each format's header says, and what's refused */
static void headers(void)
{
	unsigned char b[512];
	size_t n;
	/* PNG: IHDR's size; the chunks up to the first IDAT are walked, and an acTL there is APNG */
	n = png_start(b, 640, 480);
	n += png_chunk(b + n, "tEXt", "k\0v", 3);
	n += png_chunk(b + n, "IDAT", NULL, 0);
	CHECK(measures(b, n, "image/png", 640, 480));
	n = png_start(b, 640, 480);
	n += png_chunk(b + n, "acTL", "\0\0\0\2\0\0\0\0", 8);
	n += png_chunk(b + n, "IDAT", NULL, 0);
	CHECK(measures(b, n, NULL, 0, 0));
	n = png_start(b, 640, 480); /* no IDAT */
	CHECK(measures(b, n, NULL, 0, 0));
	n = png_start(b, 640, 480);
	n += png_chunk(b + n, "tEXt", "k\0v", 3);
	CHECK(measures(b, n - 1, NULL, 0, 0)); /* a chunk past the end */
	n = png_start(b, 0, 480);
	n += png_chunk(b + n, "IDAT", NULL, 0);
	CHECK(measures(b, n, NULL, 0, 0));

	/* JPEG: segments by their length, so a frame inside an Exif thumbnail isn't the one;
	 * fill bytes before a marker; a progressive frame (SOF2) */
	static const unsigned char jpg[] = "\xff\xd8"
		"\xff\xe1\x00\x0e" "Exif\0\0" "\xff\xc0\x00\x11\x08\x00"
		"\xff\xff\xc2\x00\x11\x08\x00\x30\x00\x40\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01";
	CHECK(measures(jpg, sizeof jpg - 1, "image/jpeg", 64, 48));
	unsigned char j[64];
	memcpy(j, jpg, sizeof jpg - 1);
	j[24] = j[25] = 0; /* height from a DNL marker */
	CHECK(measures(j, sizeof jpg - 1, NULL, 0, 0));
	memcpy(j, jpg, sizeof jpg - 1);
	j[20] = 0xc5; /* differential (hierarchical) */
	CHECK(measures(j, sizeof jpg - 1, NULL, 0, 0));
	CHECK(measures("\xff\xd8\xff\xda\x00\x02", 6, NULL, 0, 0)); /* scan data before a frame */
	CHECK(measures("\xff\xd8\xff\xe1\x00\x40" "Exif", 10, NULL, 0, 0)); /* past the end */
	CHECK(measures(jpg, 20, NULL, 0, 0));

	/* WebP: lossy (14 bits, the scale bits left out), lossless, extended with its canvas */
	unsigned char w[64];
	memcpy(w, "RIFF\0\0\0\0WEBPVP8 \x0a\0\0\0\0\0\0\x9d\x01\x2a", 26);
	w[26] = 320 & 255;
	w[27] = 320 >> 8 | 0x40;
	w[28] = 240;
	w[29] = 0;
	CHECK(measures(w, 30, "image/webp", 320, 240));
	w[24] = 0x2b; /* not a key frame's start code */
	CHECK(measures(w, 30, NULL, 0, 0));
	memcpy(w + 12, "VP8L\x05\0\0\0\x2f", 9);
	le32_put(w + 21, 399 | 299u << 14);
	CHECK(measures(w, 25, "image/webp", 400, 300));
	memcpy(w + 12, "VP8X\x0a\0\0\0\x10\0\0\0", 12);
	w[24] = 999 & 255, w[25] = 999 >> 8, w[26] = 0;
	w[27] = 749 & 255, w[28] = 749 >> 8, w[29] = 0;
	memcpy(w + 30, "VP8L\x05\0\0\0\x2f", 9);
	le32_put(w + 39, 999 | 749u << 14);
	CHECK(measures(w, 43, "image/webp", 1000, 750));
	w[20] |= 0x02; /* the animation flag */
	CHECK(measures(w, 43, NULL, 0, 0));
	w[20] &= ~0x02;
	memcpy(w + 30, "ANIM", 4); /* an animation's chunk without the flag */
	CHECK(measures(w, 43, NULL, 0, 0));
	CHECK(measures("RIFF\0\0\0\0WAVEfmt \x10\0\0\0", 20, NULL, 0, 0));

	/* GIF: one image; its extent past the logical screen counts; a second image is animation */
	static const unsigned char gif[] = "GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff"
		"\x21\xf9\x04\x01\x00\x00\x00\x00\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00\x3b";
	CHECK(measures(gif, sizeof gif - 1, "image/gif", 1, 1));
	unsigned char g[128];
	memcpy(g, gif, sizeof gif - 1);
	g[32] = 10;
	g[34] = 5;
	CHECK(measures(g, sizeof gif - 1, "image/gif", 10, 5));
	memcpy(g, gif, sizeof gif - 1);
	memcpy(g + 42, gif + 19, sizeof gif - 1 - 19); /* the frame again, then the trailer */
	CHECK(measures(g, 42 + sizeof gif - 1 - 19, NULL, 0, 0));

	/* other formats, and nothing */
	CHECK(measures("BM\x36\0\0\0\0\0\0\0\x36\0\0\0", 14, NULL, 0, 0));
	static const char svg[] = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
	CHECK(measures(svg, sizeof svg - 1, NULL, 0, 0));
	CHECK(measures("", 0, NULL, 0, 0));
}

static void image_limits(void)
{
	int tw = 0, th = 0;
	/* a long thin source for a cover slot decodes at its own size, never slot x aspect */
	CHECK(img_decode_size(1, 4000, 300, 300, false, IMAGE_MAX_SOURCE_PX, &tw, &th));
	CHECK(tw == 1 && th == 4000);
	/* a photo shrinks to the fit: cover overflows one side (cropped later), contain fits */
	CHECK(img_decode_size(6000, 4000, 132, 78, false, IMAGE_MAX_SOURCE_PX, &tw, &th));
	CHECK(tw == 132 && th == 88);
	CHECK(img_decode_size(6000, 4000, 132, 78, true, IMAGE_MAX_SOURCE_PX, &tw, &th));
	CHECK(tw == 117 && th == 78);
	/* small sources aren't enlarged at decoding */
	CHECK(img_decode_size(100, 100, 300, 300, false, IMAGE_MAX_SOURCE_PX, &tw, &th));
	CHECK(tw == 100 && th == 100);
	/* the pixel limit: 40 million is in, one row more is out; 16000 x 16000 is out */
	CHECK(img_decode_size(8000, 5000, 300, 300, false, IMAGE_MAX_SOURCE_PX, &tw, &th));
	CHECK(!img_decode_size(8000, 5001, 300, 300, false, IMAGE_MAX_SOURCE_PX, &tw, &th));
	CHECK(!img_decode_size(16000, 16000, 300, 300, false, IMAGE_MAX_SOURCE_PX, &tw, &th));
	CHECK(!img_decode_size(0, 10, 300, 300, false, IMAGE_MAX_SOURCE_PX, &tw, &th));

	/* a 1 x 4000 PNG, white with a black band in the middle rows, in a 300 x 300 cover slot:
	 * the middle is kept, and decoding it costs little (it took about 1 GB) */
	char p[2048];
	snprintf(p, sizeof p, "%s/thin.png", T);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 4000);
	cairo_t *cr = cairo_create(s);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	cairo_rectangle(cr, 0, 1990, 1, 20);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_fill(cr);
	cairo_destroy(cr);
	CHECK(cairo_surface_write_to_png(s, p) == CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy(s);
	long before = peak_rss_kb();
	unsigned char *g = prepare_file(p, 300, 300, false, IMAGE_MAX_SOURCE_PX);
	CHECK(g != NULL);
	CHECK(peak_rss_kb() - before < 100 * 1024);
	int darkest = 0, lightest = 0;
	for (int i = 0; g && i < 300 * 300; i++) {
		darkest += g[i] < 8;
		lightest = g[i] > lightest ? g[i] : lightest;
	}
	CHECK_INT(darkest, 300 * 300);
	CHECK(lightest < 8);
	free(g);

	/* a 16000 x 16000 8-bit grey PNG is refused before decoding: told to stop, glycin
	 * decoded it all the same (about 1 GB). Its 3000 x 3000 twin decodes, all black, so the
	 * file is one the decoder reads. */
	snprintf(p, sizeof p, "%s/grey.png", T);
	grey_png(p, 3000, 3000);
	g = prepare_file(p, 300, 300, false, IMAGE_MAX_SOURCE_PX);
	CHECK(g != NULL);
	darkest = 0;
	for (int i = 0; g && i < 300 * 300; i++)
		darkest += g[i] == 0;
	CHECK_INT(darkest, 300 * 300);
	free(g);
	grey_png(p, 16000, 16000);
	before = peak_rss_kb();
	CHECK(prepare_file(p, 300, 300, false, IMAGE_MAX_SOURCE_PX) == NULL);
	CHECK(peak_rss_kb() - before < 100 * 1024);

	/* the loader is the one for the measured type: a JPEG and a GIF decode */
	GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, false, 8, 64, 48);
	gdk_pixbuf_fill(pb, 0xffffffff);
	GdkPixbuf *half = gdk_pixbuf_new_subpixbuf(pb, 0, 0, 32, 48);
	gdk_pixbuf_fill(half, 0x000000ff);
	g_object_unref(half);
	gchar *jbuf = NULL;
	gsize jlen = 0;
	CHECK(gdk_pixbuf_save_to_buffer(pb, &jbuf, &jlen, "jpeg", NULL, "quality", "95", NULL));
	g_object_unref(pb);
	CHECK(measures(jbuf, jlen, "image/jpeg", 64, 48));
	snprintf(p, sizeof p, "%s/half.jpg", T);
	FILE *f = fopen(p, "wb");
	CHECK(f && fwrite(jbuf, 1, jlen, f) == jlen && fclose(f) == 0);
	g_free(jbuf);
	g = prepare_file(p, 32, 24, true, IMAGE_MAX_SOURCE_PX);
	CHECK(g != NULL);
	CHECK(g && g[12 * 32 + 4] < 32 && g[12 * 32 + 27] > 224);
	free(g);
	static const unsigned char gif[] = "GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff"
		"\x21\xf9\x04\x01\x00\x00\x00\x00\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00\x3b";
	snprintf(p, sizeof p, "%s/dot.gif", T);
	f = fopen(p, "wb");
	CHECK(f && fwrite(gif, 1, sizeof gif - 1, f) == sizeof gif - 1 && fclose(f) == 0);
	g = prepare_file(p, 4, 4, false, IMAGE_MAX_SOURCE_PX);
	CHECK(g != NULL);
	free(g);

	/* over the limit, a well-formed image is refused by the loader in use (the limit is
	 * passed small here); under it, the same file decodes */
	snprintf(p, sizeof p, "%s/meadow.png", T);
	picture(p, 200, 150, 1);
	CHECK(prepare_file(p, 100, 75, false, 200 * 150 - 1) == NULL);
	g = prepare_file(p, 100, 75, false, 200 * 150);
	CHECK(g != NULL);
	free(g);
}

/* ---- Arrange by taps and keys, the layout file after each -------------------------- */

#define CHECK_LAYOUT(lay, tags) do { char _l[2048], _u[512]; file_layout(_l, sizeof _l, NULL, NULL); \
	uuid_tags(_u, sizeof _u); CHECK_STR(_l, lay); CHECK_STR(_u, tags); } while (0)

static bool tap_ab(struct home *h, enum act a, int x, int y)
{
	for (int i = 0; i < h->nhits; i++)
		if (h->hits[i].act == a && h->hits[i].a == x && h->hits[i].b == y)
			return tap_index(h, i);
	fprintf(stderr, "test_home: no hit for act %d (%d, %d)\n", a, x, y);
	return false;
}

/* the page that shows card i (Arrange) */
static void page_with(struct home *h, int card)
{
	for (int p = 0; p < 6 && !card_on_page(h, card); p++)
		show_page(h, p);
}

/* removes the card at i, as Arrange's × does */
static void remove_card(struct home *h, int i, char *d, size_t n)
{
	page_with(h, i);
	CHECK(act(h, A_CTL_REMOVE, i, d, n));
	CHECK(strchr(d, '?') == NULL);
}

/* Add widget's card, then the gallery row's Add */
static void add_from_gallery(struct home *h, int row, char *d, size_t n)
{
	for (int p = 0; p < 6 && find_hit(h, A_ADD_CARD, -1, NULL) < 0; p++)
		show_page(h, p);
	CHECK(act(h, A_ADD_CARD, -1, d, n));
	CHECK(h->sheet == SHEET_GALLERY);
	CHECK(gallery_find(h, row));
	CHECK(act(h, A_GAL_ADD, row, d, n));
	CHECK(strchr(d, '?') == NULL);
}

static const struct value *edit_row(const struct card *c, const char *label, int *item, int *row)
{
	const struct value *dsc = c->edit_desc;
	for (size_t i = 0; dsc && i < dsc->u.a.n; i++) {
		const struct value *rows = v_map_get(dsc->u.a.v[i], "rows");
		for (size_t k = 0; rows && k < rows->u.a.n; k++) {
			const struct value *lb = v_map_get(rows->u.a.v[k], "label");
			if (lb && lb->t == V_STR && !strcmp(lb->u.s.p, label)) {
				*item = (int)i;
				*row = (int)k;
				return rows->u.a.v[k];
			}
		}
	}
	return NULL;
}

static const char *saved_prop(int idx, const char *key, char *buf, size_t n)
{
	struct home_conf c;
	char err[256], p[2048];
	conf_path(p, sizeof p);
	buf[0] = 0;
	if (conf_load(p, &c, err, sizeof err) && (size_t)idx < c.n) {
		const struct value *v = v_map_get(c.w[idx].props, key);
		if (v && v->t == V_STR)
			snprintf(buf, n, "%s", v->u.s.p);
	}
	conf_free(&c);
	return buf;
}

static void arrange(struct zygote *z)
{
	setenv("GLASSINE_HOME_VOICE", "white", 1);
	reset();
	struct home h;
	struct home_io io = { .commit = on_commit };
	home_init(&h, z, &io);
	ui_draw_init();
	home_resize(&h, 702, 884, 1);
	home_appear(&h);
	loop(&h, true, 0, -1, NULL);
	home_paint(&h, true);
	char d[512], b[64];
	const char *L0 = "reading:w notes-recent:h calendar-next:h favourites:2 package-updates:h sky:h all-apps:3 "
			 "app-category:2 photo-frame:w";

	CHECK(act(&h, A_ARRANGE, -1, d, sizeof d));
	CHECK(h.edit);
	CHECK(strchr(d, '?') == NULL); /* every changed pixel is in a region */
	for (int p = 0; p < 3; p++) {
		show_page(&h, p);
		check_swaps_offered(&h);
	}
	show_page(&h, 0);

	/* swap, by keys: Tab to the swap between Notes and Calendar, Enter */
	int idx[MAX_HITS], target = -1;
	for (int t = 0; t < 64 && target < 0; t++) {
		home_key(&h, K_TAB);
		home_tick(&h);
		int n = 0;
		for (int i = 0; i < h.nhits; i++)
			if (h.hits[i].focus)
				idx[n++] = i;
		const struct hit *f = h.focus >= 0 && h.focus < n ? &h.hits[idx[h.focus]] : NULL;
		if (f && f->act == A_SWAP && f->a == C_NOTES && f->b == C_CAL)
			target = h.focus;
	}
	CHECK(target >= 0);
	commits_before = h.commits;
	home_key(&h, K_ENTER);
	home_tick(&h);
	CHECK_INT(h.commits - commits_before, 1);
	damage_names(&h, d, sizeof d);
	CHECK_STR(d, "card1 card2");
	CHECK_LAYOUT("reading:w calendar-next:h notes-recent:h favourites:2 package-updates:h sky:h all-apps:3 "
		     "app-category:2 photo-frame:w", "1 3 2 4 5 6 7 8 9");
	CHECK_INT(h.focus, target); /* the ring stays */
	home_key(&h, K_ESCAPE); /* nothing open: Escape leaves Arrange */
	home_tick(&h);
	CHECK(!h.edit);
	CHECK(act(&h, A_ARRANGE, -1, d, sizeof d));
	/* and back, by a tap */
	CHECK(act(&h, A_SWAP, C_NOTES, d, sizeof d));
	CHECK_STR(d, "card1 card2");
	CHECK_LAYOUT(L0, "1 2 3 4 5 6 7 8 9");

	/* size: an app widget cycles its rows, a script widget its sizes, one with one size has none */
	static const char *const fav_rows[] = { "3", "5", "1", "2" };
	for (int i = 0; i < 4; i++) {
		page_with(&h, C_FAVS);
		CHECK(act(&h, A_CTL_SIZE, C_FAVS, d, sizeof d));
		CHECK(strchr(d, '?') == NULL && !strstr(d, "all"));
		CHECK(card_on_page(&h, C_FAVS)); /* the page follows the card */
		check_swaps_offered(&h);
		char want[600];
		snprintf(want, sizeof want, "reading:w notes-recent:h calendar-next:h favourites:%s package-updates:h sky:h "
			 "all-apps:3 app-category:2 photo-frame:w", fav_rows[i]);
		CHECK_LAYOUT(want, "1 2 3 4 5 6 7 8 9");
	}
	show_page(&h, 0);
	CHECK(act(&h, A_CTL_SIZE, C_READING, d, sizeof d));
	CHECK(strchr(d, '?') == NULL);
	CHECK_LAYOUT("reading:h notes-recent:h calendar-next:h favourites:2 package-updates:h sky:h all-apps:3 "
		     "app-category:2 photo-frame:w", "1 2 3 4 5 6 7 8 9");
	CHECK(act(&h, A_CTL_SIZE, C_READING, d, sizeof d));
	CHECK_LAYOUT(L0, "1 2 3 4 5 6 7 8 9");
	page_with(&h, C_SKY);
	CHECK(card_on_page(&h, C_SKY));
	CHECK(find_hit(&h, A_CTL_SIZE, C_SKY, NULL) < 0);
	CHECK(find_hit(&h, A_CTL_SETTINGS, C_SKY, NULL) < 0); /* no settings, no button */

	/* a page swap: page 1's widgets and page 2's trade places; the arranged page stays in view */
	show_page(&h, 0);
	CHECK(find_hit(&h, A_PAGE_SWAP, -1, NULL) >= 0);
	commits_before = h.commits;
	CHECK(tap_ab(&h, A_PAGE_SWAP, 0, 1));
	CHECK_INT(h.commits - commits_before, 1);
	damage_names(&h, d, sizeof d);
	CHECK(strstr(d, "header") && strstr(d, "page") && !strchr(d, '?')); /* the pager reads 2/3 */
	CHECK_INT(h.page, 1);
	CHECK_LAYOUT("all-apps:3 app-category:2 reading:w notes-recent:h calendar-next:h favourites:2 package-updates:h "
		     "sky:h photo-frame:w", "7 8 1 2 3 4 5 6 9");
	commits_before = h.commits;
	CHECK(tap_ab(&h, A_PAGE_SWAP, 1, 0));
	CHECK_INT(h.commits - commits_before, 1);
	CHECK_INT(h.page, 0);
	CHECK_LAYOUT(L0, "1 2 3 4 5 6 7 8 9");
	/* a peek turns to its page */
	CHECK(act(&h, A_PEEK, 1, d, sizeof d));
	CHECK_INT(h.page, 1);

	/* remove: All apps too; the gallery offers it again */
	remove_card(&h, C_ALL, d, sizeof d);
	CHECK_LAYOUT("reading:w notes-recent:h calendar-next:h favourites:2 package-updates:h sky:h app-category:2 "
		     "photo-frame:w", "1 2 3 4 5 6 8 9");
	add_from_gallery(&h, 1, d, sizeof d);
	CHECK(h.sheet == SHEET_NONE);
	CHECK_LAYOUT("reading:w notes-recent:h calendar-next:h favourites:2 package-updates:h sky:h app-category:2 "
		     "photo-frame:w all-apps:3", "1 2 3 4 5 6 8 9 new");
	CHECK(card_on_page(&h, 8)); /* Add turns to the new widget's page */
	CHECK(act(&h, A_ADD_CARD, -1, d, sizeof d));
	CHECK(find_hit(&h, A_GAL_ADD, 0, NULL) < 0 && find_hit(&h, A_GAL_ADD, 1, NULL) < 0); /* on Home: a check */
	CHECK(find_hit(&h, A_GAL_ADD, 2, NULL) >= 0);                                         /* App category: again */
	home_key(&h, K_ESCAPE);
	home_tick(&h);
	CHECK(h.sheet == SHEET_NONE && h.edit);

	/* built in, from an app, your own: removed, then added back from the gallery at the end */
	static const struct { const char *ref; const char *name; int card; } re[3] = {
		{ "builtin/reading", "reading:w", 0 }, { "system/photo-frame", "photo-frame:w", 6 }, { "user/sky", "sky:h", 4 },
	};
	char lay[2048];
	for (int i = 0; i < 3; i++) {
		int row = gallery_row(&h, re[i].ref);
		CHECK(row >= 3);
		CHECK_STR(h.conf.w[re[i].card].widget, re[i].ref);
		remove_card(&h, re[i].card, d, sizeof d);
		add_from_gallery(&h, row, d, sizeof d);
		file_layout(lay, sizeof lay, NULL, NULL);
		const char *last = strrchr(lay, ' ');
		CHECK_STR(last ? last + 1 : lay, re[i].name);
		CHECK(card_on_page(&h, (int)h.conf.n - 1));
	}
	CHECK_LAYOUT("notes-recent:h calendar-next:h favourites:2 package-updates:h app-category:2 all-apps:3 "
		     "reading:w photo-frame:w sky:h", "2 3 4 5 8 new new new new");

	/* App category a second time: its settings open at once; Office applies at once, to it alone */
	add_from_gallery(&h, 2, d, sizeof d);
	int nc = (int)h.conf.n - 1;
	CHECK_STR(h.conf.w[nc].widget, "home/app-category");
	CHECK(h.sheet == SHEET_SETTINGS && h.sheet_card == nc);
	settle(&h);
	CHECK(h.cards[nc].edit_desc != NULL);
	int item = -1, row = -1;
	const struct value *office = edit_row(&h.cards[nc], "Office", &item, &row);
	CHECK(office && !v_map_get(office, "selected"));
	CHECK(find_hit(&h, A_SET_ROW, item, NULL) >= 0);
	commits_before = h.commits;
	CHECK(tap_ab(&h, A_SET_ROW, item, row));
	CHECK_INT(h.commits - commits_before, 1);
	damage_names(&h, d, sizeof d);
	char want[64];
	snprintf(want, sizeof want, "card%d sheet", nc); /* the card behind shows Office; the sheet's subtitle */
	CHECK_STR(d, want);
	CHECK_STR(saved_prop(nc, "category", b, sizeof b), "Office");
	CHECK_STR(saved_prop(4, "category", b, sizeof b), "Games");
	/* the sheet marks the new choice once its script ran again: one more commit, the sheet only */
	commits_before = h.commits;
	loop(&h, true, 1500, -1, NULL);
	CHECK_INT(h.commits - commits_before, 1);
	damage_names(&h, d, sizeof d);
	CHECK_STR(d, "sheet");
	office = edit_row(&h.cards[nc], "Office", &item, &row);
	CHECK(office && v_map_get(office, "selected"));
	CHECK_LAYOUT("notes-recent:h calendar-next:h favourites:2 package-updates:h app-category:2 all-apps:3 "
		     "reading:w photo-frame:w sky:h app-category:2", "2 3 4 5 8 new new new new new");

	/* closing the sheet, turning the screen and leaving Arrange write nothing */
	char before[2048];
	file_layout(before, sizeof before, NULL, NULL);
	home_key(&h, K_ESCAPE);
	home_tick(&h);
	CHECK(h.sheet == SHEET_NONE && h.edit);
	/* landscape: two lanes; swaps are offered by the same rule */
	home_resize(&h, 936, 702, 1);
	settle(&h);
	for (int p = 0; p < 4; p++) {
		show_page(&h, p);
		check_swaps_offered(&h);
	}
	CHECK(swaps_refused > 0); /* some shared edges were rightly not offered */
	home_key(&h, K_ESCAPE);
	home_tick(&h);
	CHECK(!h.edit);
	file_layout(lay, sizeof lay, NULL, NULL);
	CHECK_STR(lay, before);
	home_free(&h);
}

int main(void)
{
	setup();
	/* the zygote first, before any thread (sandbox.h) */
	struct zygote z, z2; /* z2 for helper_gone, which kills it */
	if (!zygote_start(&z) || !zygote_start(&z2)) {
		perror("zygote");
		return 1;
	}
	static const char *const voices[] = { "white", "editorial", "stationery" };
	for (int v = 0; v < 3; v++)
		for (int l = 0; l < 2; l++)
			boards(&z, voices[v], l);
	commands(&z);
	in_flight(&z);
	headers();
	image_limits();
	frames_busy();
	touch_targets(&z);
	helper_gone(&z2);
	zygote_stop(&z2);
	arrange(&z);
	zygote_stop(&z);
	printf("renders in out/renders/\n");
	return t_done("home");
}
