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
#include <stdarg.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

static bool tap(struct home *h, enum act act, int a, const char *id)
{
	int i = find_hit(h, act, a, id);
	if (i < 0) {
		fprintf(stderr, "test_home: no hit for act %d (%d, %s)\n", act, a, id ? id : "-");
		return false;
	}
	double x = h->hits[i].r.x + h->hits[i].r.w / 2, y = h->hits[i].r.y + h->hits[i].r.h / 2;
	home_press(h, x, y);
	home_release(h, x, y);
	home_tick(h);
	return true;
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

	home_free(&h);
	setenv("GLASSINE_HOME_USER_WIDGETS", was, 1);
}

int main(void)
{
	setup();
	/* the zygote first, before any thread (sandbox.h) */
	struct zygote z;
	if (!zygote_start(&z)) {
		perror("zygote");
		return 1;
	}
	static const char *const voices[] = { "white", "editorial", "stationery" };
	for (int v = 0; v < 3; v++)
		for (int l = 0; l < 2; l++)
			boards(&z, voices[v], l);
	commands(&z);
	in_flight(&z);
	zygote_stop(&z);
	printf("renders in out/renders/\n");
	return t_done("home");
}
