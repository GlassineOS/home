/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * glassine-home's core: the model (layout file, catalog, apps, widget runs), the
 * scene (painted with GlassineOS/ui into an offscreen canvas), input and the
 * decisions when to paint. The Wayland side (wl.c) and the tests only feed it sizes,
 * input and time, and get back damage rectangles to commit.
 *
 * Painting: every paint draws the whole scene into a fresh canvas and compares it
 * with the last one in tiles. Changed tiles are grouped by the smallest region that
 * holds them (an image slot, a card, the header, ...) and that region's rectangle is
 * damaged, so a changed card is one rectangle and an image filling in is only its
 * slot. Nothing changed = no commit. All damage of one paint goes in one commit.
 *
 * When: input paints at once; everything else (widget output, images, the clock,
 * apps or the voice changing) waits until at least a second has passed since the
 * last background paint. Nothing animates.
 */
#ifndef GH_HOME_H
#define GH_HOME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cairo.h>
#include <glassine-ui.h>

#include "apps.h"
#include "catalog.h"
#include "conf.h"
#include "desc.h"
#include "pack.h"
#include "sandbox.h"
#include "sched.h"

#define HOLD_MS 550
#define BG_PAINT_MS 1000
#define DEBOUNCE_QUIET_MS 500
#define DEBOUNCE_MAX_MS 3000
#define MAX_HITS 512
#define MAX_REGIONS 128
#define MAX_DAMAGE 64
#define TOUCH_MIN 48 /* FR-041: a press counts this far around a smaller control (hit_at) */
#define IMAGE_MAX_BYTES (20 * 1024 * 1024)
/* sources over this many pixels aren't decoded (dashed frame): memory follows the pixels, not the bytes */
#define IMAGE_MAX_SOURCE_PX (40LL * 1000 * 1000)

struct rect { double x, y, w, h; };

enum native { NAT_NONE, NAT_FAV, NAT_ALL, NAT_CAT };

enum act {
	A_NONE,
	A_PAGE_PREV, A_PAGE_NEXT, A_ARRANGE, A_DONE,
	A_APP,            /* id: tap = open on a new screen, hold = the app menu */
	A_APPS_PREV, A_APPS_NEXT, /* a: card */
	A_WBUTTON,        /* a: card, b: button index in its description */
	A_CTL_SETTINGS, A_CTL_SIZE, A_CTL_REMOVE, /* a: card */
	A_ADD_CARD, A_SWAP /* a, b: cards */, A_PAGE_SWAP /* a, b: pages */, A_PEEK /* a: page */,
	A_SHEET_CLOSE, A_SHEET_OUTSIDE, A_SHEET_PREV, A_SHEET_NEXT,
	A_MENU_NEW, A_MENU_HERE, A_MENU_FAV, A_MENU_CONF,
	A_GAL_ADD,        /* a: gallery row */
	A_SET_ROW,        /* a: item, b: row of a list in the settings description */
	A_SET_BUTTON,     /* a: item */
};

struct hit {
	struct rect r;
	enum act act;
	int a, b;
	char id[APP_ID_MAX];
	bool focus;       /* reachable with Tab */
	double radius;    /* of the focus ring's shape */
};

enum region_kind { R_HEADER, R_PAGE, R_CARD, R_SLOT, R_HINT, R_SHEET };
struct region { struct rect r; enum region_kind kind; int card; };

/* An image slot's picture, prepared off the main thread (images.c). */
struct img {
	char key[64];          /* hash of source, its date, slot size, fit and dithering */
	int w, h;              /* device px */
	cairo_surface_t *surf; /* NULL until ready */
	bool failed, queued;
};

struct card {
	char uuid[37];
	enum native nat;
	const struct catalog_entry *ce; /* script widgets; NULL when the widget is gone */
	const struct manifest *m;      /* the widget's manifest, or the native one's (settings); NULL for none */
	const char *dir;
	struct value *props;           /* effective settings */
	struct value *desc;            /* last good output */
	struct value *edit_desc;       /* the settings sheet's description */
	struct widget_state st;
	char fail[200];                /* why the last run failed */
	char *files[BROKER_MAX_FILES];
	size_t nfiles;
	struct debounce db;
	bool want_run, want_edit;
	enum trigger trig;
	int apage;                     /* an app widget's own page */
	struct img img[DESC_MAX_IMAGES];
};

enum sheet { SHEET_NONE, SHEET_MENU, SHEET_GALLERY, SHEET_SETTINGS };

/* What the outside does for Home. */
struct home_io {
	/* the damage of one paint, in canvas device px; one commit */
	void (*commit)(void *ud, const struct rect *r, int n);
	void *ud;
};

struct home {
	/* size: logical (design) px and device px per logical px */
	int W, H, S;
	enum orient orient;
	const UiVoice *v;
	char voice_name[32];
	bool voice_fixed;              /* GLASSINE_HOME_VOICE */

	char conf_path[1024];
	struct home_conf conf;
	struct catalog cat;
	char builtin_root[1024], system_root[1024], native_root[1024];
	struct manifest native_cat;    /* native/app-category: App category's settings */
	bool have_native_cat;
	struct apps apps;
	struct card *cards;
	size_t ncards;

	struct zygote *z;
	struct run *run;
	struct run_result rr;
	int run_card;
	bool run_edit;

	/* view */
	int page, npages;
	bool edit;
	enum sheet sheet;
	int sheet_card;
	char menu_app[APP_ID_MAX];
	int sheet_page, sheet_pages;
	int focus;                     /* index among the focusable hits, -1 none */

	/* the last paint */
	struct hit hits[MAX_HITS];
	int nhits;
	struct region regions[MAX_REGIONS];
	int nregions;
	cairo_surface_t *cur;
	int commits;                   /* counts commits (tests) */
	bool dirty_now, dirty_bg;
	long long last_bg_ms;

	/* a press */
	bool pressing, held;
	int press_hit;
	long long press_ms;
	double px, py;

	/* watching files */
	int ino;
	struct { int wd; char *dir; } watch[64];
	int nwatch;
	int wd_apps[16], wd_cat[3], wd_shellconf, wd_eink;

	/* images */
	struct imgq *iq;
	bool dither;
	char dither_pattern[16];

	long long now_s;               /* GLASSINE_HOME_NOW (tests), else 0 = the clock */
	long minute;                   /* the clock's minute as last painted */
	char launch_cmd[512], hide_cmd[512];
	struct home_io io;
};

/* Sets up everything but the zygote (started by main before any thread). */
bool home_init(struct home *h, struct zygote *z, const struct home_io *io);
void home_free(struct home *h);
void home_resize(struct home *h, int W, int H, int S);
/* Home appears: run every widget once. */
void home_appear(struct home *h);

/* The event loop: fds to poll (POLLIN), what to do when they are ready or time passes. */
int home_fds(struct home *h, int *fds, int max);
void home_ready(struct home *h, int fd);
/* Runs due work and paints when it should; returns ms until it wants to be called again. */
int home_tick(struct home *h);

/* Input, in logical px. */
void home_press(struct home *h, double x, double y);
void home_release(struct home *h, double x, double y);
void home_cancel(struct home *h);
enum key { K_TAB, K_BACKTAB, K_ENTER, K_SPACE, K_ESCAPE };
void home_key(struct home *h, enum key k);

/* Paints now if anything changed (tests: also after background changes). */
void home_paint(struct home *h, bool background_too);

/* scene.c */
void scene_paint(struct home *h, cairo_t *cr);
void scene_layout_page(struct home *h, struct rect *grid);
bool home_layout_pages(struct home *h, struct pack_pos *pos, int *npages_with_add, struct pack_pos *add);
const char *card_name(const struct home *h, const struct card *c, char *buf, size_t len);
bool card_has_settings(const struct home *h, const struct card *c);
long long home_now(const struct home *h);
bool home_is_fav(const struct home *h, const char *id);
void home_status(const struct home *h, char *title, size_t tlen, char *sub, size_t slen);

/*
 * frames.c: the bookkeeping behind wl.c's commits. A paint's damage goes to every
 * buffer (each copies what changed since it was last shown) and to the surface (what
 * changed since the last commit). A paint while both buffers are with the compositor
 * isn't committed then: it is owed, and committed when a buffer comes back.
 */
#define FRAME_BUFFERS 2
struct frames {
	struct { bool busy; struct rect dmg[MAX_DAMAGE]; int ndmg; bool all; } b[FRAME_BUFFERS];
	struct rect sdmg[MAX_DAMAGE];
	int nsdmg;
	bool sall;
	bool owed;
};
void frames_init(struct frames *f);
void frames_damage(struct frames *f, const struct rect *r, int n);
/* A free buffer, or -1: then the frame is owed. */
int frames_pick(struct frames *f);
/* Buffer i was committed: it is busy, and nothing is left to copy or damage. */
void frames_shown(struct frames *f, int i);
void frames_released(struct frames *f, int i);

/* images.c: a worker thread that prepares image slots */
struct imgq *imgq_start(void);
void imgq_stop(struct imgq *q);
int imgq_fd(struct imgq *q);
/* Requests slot i of card c (source, slot size in device px); fills c->img[i] when ready. */
void img_request(struct home *h, struct card *c, int i, const struct value *item, int lw, int lh);
/* Collects finished images; returns true when one arrived. */
bool imgq_collect(struct home *h);
void img_clear(struct img *im);
/* The book's freedesktop thumbnail (large first), for cover: sources. */
bool img_cover_path(const char *book, char *out, size_t len);
/*
 * The size a sw x sh source is decoded at for a w x h slot: shrunk to the fit (cover or
 * contain) when that is smaller, else the source's own size, never larger. Cover is
 * cropped from it afterwards, so a long thin source never decodes to slot x aspect.
 * False for a source over max_px pixels (IMAGE_MAX_SOURCE_PX; tests pass less).
 */
bool img_decode_size(int sw, int sh, int w, int h, bool contain, long long max_px, int *tw, int *th);
/*
 * A source's format (a MIME type) and size, from its header alone: PNG, JPEG, WebP or GIF.
 * NULL for any other format, for an animation (APNG, an animated WebP, a GIF of more than
 * one image) and for a header that doesn't give the size.
 */
const char *img_measure(const unsigned char *d, size_t n, int *w, int *h);
/* The worker's job for one slot: w x h greys (0 = black), or NULL (not measured, broken, too large). */
unsigned char *img_prepare(int fd, int w, int h, bool contain, int dmode, long long max_px);

#endif
