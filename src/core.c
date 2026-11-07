// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * glassine-home's core: the model, widget runs on the event loop, input, actions and
 * when to paint (see home.h).
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "home.h"
#include "manifest.h"

#define INO_DATA (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_ATTRIB)

static long long mono_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

long long home_now(const struct home *h)
{
	return h->now_s ? h->now_s : (long long)time(NULL);
}

static long tz_offset(long long now)
{
	time_t t = (time_t)now;
	struct tm tm;
	localtime_r(&t, &tm);
	return tm.tm_gmtoff;
}

static const char *env_or(const char *name, const char *def)
{
	const char *v = getenv(name);
	return v && *v ? v : def;
}

/* ---- the voice and the E-ink dithering setting -------------------------------- */

static void shell_conf_dir(char *out, size_t len)
{
	const char *hm = getenv("HOME");
	snprintf(out, len, "%s/.config/glassine-shell", hm ? hm : "/");
}

static void load_voice(struct home *h)
{
	char name[32] = "white";
	const char *e = getenv("GLASSINE_HOME_VOICE");
	if (e && *e) {
		snprintf(name, sizeof name, "%s", e);
		h->voice_fixed = true;
	} else {
		char p[1100], d[1024];
		shell_conf_dir(d, sizeof d);
		snprintf(p, sizeof p, "%s/theme", d);
		FILE *f = fopen(p, "re");
		if (f) {
			if (fscanf(f, "%31s", name) != 1)
				snprintf(name, sizeof name, "white");
			fclose(f);
		}
	}
	snprintf(h->voice_name, sizeof h->voice_name, "%s", name);
	h->v = ui_voice_find(name);
}

static void load_dither(struct home *h)
{
	h->dither = true;
	snprintf(h->dither_pattern, sizeof h->dither_pattern, "blue");
	FILE *f = fopen(env_or("GLASSINE_HOME_EINK_CONF", "/etc/glassine/eink.conf"), "re");
	if (!f)
		return;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *k = line[0] == '@' ? line + 1 : line, *eq = strchr(k, '=');
		if (!eq)
			continue;
		*eq = 0;
		if (!strcmp(k, "dither"))
			h->dither = strcmp(eq + 1, "off") != 0;
		else if (!strcmp(k, "dither_pattern") &&
			 (!strcmp(eq + 1, "blue") || !strcmp(eq + 1, "bayer") || !strcmp(eq + 1, "dizzy")))
			snprintf(h->dither_pattern, sizeof h->dither_pattern, "%s", eq + 1);
	}
	fclose(f);
}

/* ---- cards: the layout file's widgets with their runtime state ----------------- */

static enum native native_of(const char *ref)
{
	if (!strcmp(ref, "home/favourites"))
		return NAT_FAV;
	if (!strcmp(ref, "home/all-apps"))
		return NAT_ALL;
	if (!strcmp(ref, "home/app-category"))
		return NAT_CAT;
	return NAT_NONE;
}

static void card_free(struct card *c)
{
	v_free(c->props);
	v_free(c->desc);
	v_free(c->edit_desc);
	for (size_t i = 0; i < c->nfiles; i++)
		free(c->files[i]);
	for (int i = 0; i < DESC_MAX_IMAGES; i++)
		img_clear(&c->img[i]);
	memset(c, 0, sizeof *c);
}

static void card_props(struct home *h, struct card *c, const struct instance *in)
{
	v_free(c->props);
	if (c->m)
		c->props = props_effective(c->m, in->props);
	else
		c->props = in->props ? v_dup(in->props) : v_map();
}

/* Rebuilds cards[] from the layout, keeping each instance's runtime state. */
static void cards_sync(struct home *h)
{
	struct card *nc = calloc(h->conf.n ? h->conf.n : 1, sizeof *nc);
	if (!nc)
		return;
	/* a run in flight and the open settings sheet follow their instance, not its index:
	 * a remove, a swap or an outside change of home.conf reorders */
	char run_uuid[37] = "", sheet_uuid[37] = "";
	if (h->sheet_card >= 0 && (size_t)h->sheet_card < h->ncards)
		snprintf(sheet_uuid, sizeof sheet_uuid, "%s", h->cards[h->sheet_card].uuid);
	if (h->run && h->run_card >= 0 && (size_t)h->run_card < h->ncards)
		snprintf(run_uuid, sizeof run_uuid, "%s", h->cards[h->run_card].uuid);
	for (size_t i = 0; i < h->conf.n; i++) {
		const struct instance *in = &h->conf.w[i];
		struct card *c = &nc[i];
		bool kept = false;
		for (size_t j = 0; j < h->ncards; j++)
			if (!strcmp(h->cards[j].uuid, in->uuid) && h->cards[j].uuid[0]) {
				*c = h->cards[j];
				memset(&h->cards[j], 0, sizeof h->cards[j]);
				kept = true;
				break;
			}
		if (!kept) {
			snprintf(c->uuid, sizeof c->uuid, "%s", in->uuid);
			c->want_run = true;
			c->trig = TRIGGER_APPEAR;
		}
		c->nat = native_of(in->widget);
		c->ce = NULL;
		c->m = NULL;
		c->dir = NULL;
		if (c->nat == NAT_CAT && h->have_native_cat) {
			c->m = &h->native_cat;
			c->dir = h->native_root;
		} else if (c->nat == NAT_NONE) {
			const struct catalog_entry *e = catalog_find(&h->cat, in->widget);
			if (e && e->ok) {
				c->ce = e;
				c->m = &e->m;
				c->dir = e->dir;
				if (e->m.every_s && !c->st.next_due)
					c->st.next_due = sched_next(home_now(h), e->m.every_s, tz_offset(home_now(h)));
			}
		}
		card_props(h, c, in);
	}
	for (size_t j = 0; j < h->ncards; j++)
		card_free(&h->cards[j]);
	free(h->cards);
	h->cards = nc;
	h->ncards = h->conf.n;
	h->run_card = run_uuid[0] ? conf_find(&h->conf, run_uuid) : -1; /* -1: removed, the result is dropped */
	h->sheet_card = sheet_uuid[0] ? conf_find(&h->conf, sheet_uuid) : -1;
	if (h->sheet == SHEET_SETTINGS && h->sheet_card < 0)
		h->sheet = SHEET_NONE;
}

/* ---- the layout file, shared with Settings (docs/home-conf.md) ----------------------- */

static int watch_dir(struct home *h, const char *dir, uint32_t mask);

static void watch_conf(struct home *h)
{
	if (h->wd_conf >= 0)
		return;
	char d[1024];
	snprintf(d, sizeof d, "%s", h->conf_path);
	char *sl = strrchr(d, '/');
	if (sl && sl != d)
		*sl = 0;
	h->wd_conf = watch_dir(h, d, INO_DATA);
}

static bool same_as_disk(const struct home *h, const char *text, size_t len)
{
	if (!text || !h->disk_text)
		return !text && !h->disk_text;
	return len == h->disk_len && !memcmp(text, h->disk_text, len);
}

/*
 * Takes from d (the file as it is now) the favourites and/or the widgets. A widget
 * that is gone disappears; one whose widget or settings changed runs again; the page,
 * Arrange and an open sheet stay (the sheet closes when its widget went). Returns true
 * when what Home shows changed.
 */
static bool adopt(struct home *h, struct home_conf *d, bool take_fav, bool take_widgets)
{
	bool changed = false;
	if (take_fav && !conf_favourites_equal(&h->conf, d)) {
		conf_take_favourites(&h->conf, d);
		changed = true;
	}
	if (take_widgets && !conf_widgets_equal(&h->conf, d)) {
		char (*rerun)[37] = calloc(d->n ? d->n : 1, sizeof *rerun);
		size_t nr = 0;
		for (size_t i = 0; rerun && i < d->n; i++) {
			int k = conf_find(&h->conf, d->w[i].uuid);
			if (k < 0)
				continue;
			struct instance t = d->w[i];
			t.size = h->conf.w[k].size;
			if (!conf_instance_equal(&h->conf.w[k], &t))
				memcpy(rerun[nr++], d->w[i].uuid, 37);
		}
		conf_take_widgets(&h->conf, d);
		cards_sync(h);
		for (size_t i = 0; i < nr; i++) {
			int k = conf_find(&h->conf, rerun[i]);
			if (k < 0)
				continue;
			struct card *c = &h->cards[k];
			c->want_run = true;
			c->trig = TRIGGER_SETTINGS;
			c->want_edit = h->sheet == SHEET_SETTINGS && h->sheet_card == k;
			c->apage = 0;
		}
		free(rerun);
		h->focus = -1;
		changed = true;
	}
	return changed;
}

/*
 * The file changed since Home last read or wrote it, and parses: takes the parts Home
 * hasn't changed since then (all of it, unless an earlier write is still waiting for the
 * lock). Settings changes only favourites, Home's Arrange only the widgets, so the two
 * merge; when both changed the same part, Home's write wins.
 */
static bool merge_disk(struct home *h, struct home_conf *d, bool *kept_mine)
{
	struct home_conf base;
	char err[256];
	if (!h->disk_text || !conf_parse(h->disk_text, h->disk_len, &base, err, sizeof err))
		memset(&base, 0, sizeof base);
	bool fav_mine = !conf_favourites_equal(&h->conf, &base), w_mine = !conf_widgets_equal(&h->conf, &base);
	/* a file without a widget key was never laid out (Settings made it): Home's widgets stay */
	if (!d->has_widgets && h->conf.n)
		w_mine = true;
	conf_free(&base);
	h->conf.read_only = false;
	*kept_mine = fav_mine || w_mine;
	return adopt(h, d, !fav_mine, !w_mine);
}

/*
 * Writes the layout under the lock, after taking in what changed outside since Home last
 * looked. When it can't (the lock stays busy, or the write fails), the change stays in
 * memory and save_pending makes home_tick try again; nothing waits for the lock then.
 */
static void save_conf(struct home *h)
{
	char err[256];
	if (h->conf.read_only)
		return;
	bool was_pending = h->save_pending;
	int lk = conf_lock(h->conf_path, was_pending ? 0 : CONF_LOCK_WAIT_MS);
	if (lk < 0) {
		if (!was_pending)
			fprintf(stderr, "glassine-home: %s.lock is held; writing the layout when it is free\n", h->conf_path);
		h->save_pending = true;
		return;
	}
	h->save_pending = false;
	size_t len, dlen;
	enum conf_state st;
	struct home_conf d;
	char *disk = conf_read(h->conf_path, &dlen, &st, &d, err, sizeof err);
	if (!same_as_disk(h, disk, dlen)) {
		bool mine;
		if (st == CONF_OK) {
			if (merge_disk(h, &d, &mine))
				h->dirty_bg = true;
			/* what was adopted is now what Home last read: if the write below fails, a later
			 * outside change must not find Settings' favourites counted as Home's own */
			free(h->disk_text);
			h->disk_text = disk;
			h->disk_len = dlen;
			disk = NULL;
		} else if (st != CONF_MISSING) {
			/* broken, newer or unreadable from outside: Home keeps what it shows and leaves the file */
			fprintf(stderr, "glassine-home: %s; not writing over it\n", err[0] ? err : h->conf_path);
			h->conf_refused++;
			conf_free(&d);
			free(disk);
			conf_unlock(lk);
			h->save_retry_ms = 0;
			return;
		}
	}
	conf_free(&d);
	free(disk);
	char *text = conf_format(&h->conf, &len, err, sizeof err);
	if (text && conf_write_text(h->conf_path, text, len, err, sizeof err)) {
		free(h->disk_text);
		h->disk_text = text;
		h->disk_len = len;
		h->save_retry_ms = 0;
	} else {
		if (!h->save_retry_ms) /* once per run of failures */
			fprintf(stderr, "glassine-home: %s; keeping the change and trying again\n", err);
		free(text);
		h->save_pending = true;
		h->save_retry_ms = mono_ms() + CONF_RETRY_MS;
	}
	conf_unlock(lk);
	watch_conf(h);
}

/* home.conf was replaced or written in place by someone else (or by Home: then it reads the same). */
static void conf_changed(struct home *h)
{
	char err[256];
	size_t len;
	enum conf_state st;
	struct home_conf d;
	char *disk = conf_read(h->conf_path, &len, &st, &d, err, sizeof err);
	if (same_as_disk(h, disk, len)) {
		free(disk);
		return;
	}
	if (st == CONF_OK) {
		bool mine;
		if (merge_disk(h, &d, &mine))
			h->dirty_bg = true;
		free(h->disk_text);
		h->disk_text = disk;
		h->disk_len = len;
		disk = NULL;
		h->reloads++;
		h->refused_hash = 0;
		/* a change Home couldn't write (the lock, or a file it couldn't read) goes in now */
		if (mine || h->save_pending)
			save_conf(h);
	} else if (st == CONF_MISSING) {
		fprintf(stderr, "glassine-home: %s was removed; keeping the layout, written again at the next change\n",
			h->conf_path);
	} else if (!disk || hash64(disk, len) != h->refused_hash) {
		fprintf(stderr, "glassine-home: %s; keeping the layout and not writing over it\n", err[0] ? err : h->conf_path);
		h->conf_refused++;
		h->refused_hash = disk ? hash64(disk, len) : 0;
	}
	conf_free(&d);
	free(disk);
}

/*
 * A new Home (no file, or one without a widget key, as Settings creates it for its
 * favourites): Continue reading, Favourite apps and All apps; without favourites in the
 * file, the shell's old list.
 */
static void first_layout(struct home *h)
{
	if (catalog_find(&h->cat, "builtin/reading"))
		conf_add(&h->conf, "builtin/reading", SIZE_WIDE, NULL);
	conf_add(&h->conf, "home/favourites", 2, NULL);
	conf_add(&h->conf, "home/all-apps", 3, NULL);
	char d[1024], p[1100];
	shell_conf_dir(d, sizeof d);
	snprintf(p, sizeof p, "%s/favourites", d);
	FILE *f = h->conf.nfav ? NULL : fopen(p, "re");
	if (f) {
		char line[APP_ID_MAX];
		while (fgets(line, sizeof line, f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (!line[0])
				continue;
			char **nf = realloc(h->conf.favourites, (h->conf.nfav + 1) * sizeof *nf);
			if (!nf)
				break;
			h->conf.favourites = nf;
			h->conf.favourites[h->conf.nfav++] = strdup(line);
		}
		fclose(f);
	}
	save_conf(h);
}

/* ---- watching files -------------------------------------------------------------- */

static int watch_dir(struct home *h, const char *dir, uint32_t mask)
{
	return h->ino >= 0 ? inotify_add_watch(h->ino, dir, mask | IN_ONLYDIR) : -1;
}

/* A watch Home keeps for itself (inotify gives one wd per directory, shared with cards' watches). */
static bool static_wd(const struct home *h, int wd)
{
	if (wd < 0)
		return false;
	for (int i = 0; i < 16; i++)
		if (h->wd_apps[i] == wd)
			return true;
	for (int i = 0; i < 3; i++)
		if (h->wd_cat[i] == wd)
			return true;
	return wd == h->wd_shellconf || wd == h->wd_eink || wd == h->wd_conf;
}

/* Watches the parent directories of every file a card's last run read. */
static void rewatch(struct home *h)
{
	char *want[64];
	size_t nw = 0;
	for (size_t i = 0; i < h->ncards; i++) {
		char *dirs[64];
		size_t n = watch_dirs(h->cards[i].files, h->cards[i].nfiles, dirs, 64);
		for (size_t k = 0; k < n; k++) {
			bool dup = false;
			for (size_t j = 0; j < nw && !dup; j++)
				dup = !strcmp(want[j], dirs[k]);
			if (dup || nw >= 64)
				free(dirs[k]);
			else
				want[nw++] = dirs[k];
		}
	}
	for (int i = 0; i < h->nwatch;) {
		bool keep = false;
		for (size_t j = 0; j < nw && !keep; j++)
			keep = !strcmp(want[j], h->watch[i].dir);
		if (keep) {
			i++;
			continue;
		}
		if (!static_wd(h, h->watch[i].wd))
			inotify_rm_watch(h->ino, h->watch[i].wd); /* one watch per directory: keep it if Home needs it too */
		free(h->watch[i].dir);
		h->watch[i] = h->watch[--h->nwatch];
	}
	for (size_t j = 0; j < nw; j++) {
		bool have = false;
		for (int i = 0; i < h->nwatch && !have; i++)
			have = !strcmp(want[j], h->watch[i].dir);
		int wd = have ? -1 : watch_dir(h, want[j], INO_DATA);
		if (!have && wd >= 0 && h->nwatch < 64) {
			h->watch[h->nwatch].wd = wd;
			h->watch[h->nwatch++].dir = want[j];
		} else {
			free(want[j]);
		}
	}
}

static void watch_static(struct home *h)
{
	char dirs[16][1024];
	size_t nd = apps_dirs(dirs, 16);
	for (size_t i = 0; i < 16; i++)
		h->wd_apps[i] = i < nd ? watch_dir(h, dirs[i], INO_DATA) : -1;
	for (int i = 0; i < 3; i++)
		h->wd_cat[i] = h->cat.roots[i] ? watch_dir(h, h->cat.roots[i], INO_DATA) : -1;
	char d[1024];
	shell_conf_dir(d, sizeof d);
	h->wd_shellconf = h->voice_fixed ? -1 : watch_dir(h, d, INO_DATA);
	char e[1024];
	snprintf(e, sizeof e, "%s", env_or("GLASSINE_HOME_EINK_CONF", "/etc/glassine/eink.conf"));
	char *sl = strrchr(e, '/');
	if (sl && sl != e)
		*sl = 0;
	h->wd_eink = watch_dir(h, e, INO_DATA);
	watch_conf(h);
}

/* ---- setup ---------------------------------------------------------------------- */

static void rescan(struct home *h)
{
	catalog_free(&h->cat);
	catalog_scan(&h->cat, h->builtin_root, h->system_root, getenv("GLASSINE_HOME_USER_WIDGETS"));
	cards_sync(h);
}

bool home_init(struct home *h, struct zygote *z, const struct home_io *io)
{
	memset(h, 0, sizeof *h);
	h->z = z;
	h->io = *io;
	h->focus = -1;
	h->press_hit = -1;
	h->sheet_card = -1;
	h->run_card = -1;
	h->wd_conf = h->wd_shellconf = h->wd_eink = -1;
	for (int i = 0; i < 16; i++)
		h->wd_apps[i] = -1;
	for (int i = 0; i < 3; i++)
		h->wd_cat[i] = -1;
	h->ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	const char *now = getenv("GLASSINE_HOME_NOW");
	h->now_s = now ? atoll(now) : 0;
	snprintf(h->launch_cmd, sizeof h->launch_cmd, "%s", env_or("GLASSINE_HOME_LAUNCH", "glassine-shell launch"));
	snprintf(h->hide_cmd, sizeof h->hide_cmd, "%s", env_or("GLASSINE_HOME_HIDE", "glassine-swayd home hide"));
	snprintf(h->builtin_root, sizeof h->builtin_root, "%s",
		 env_or("GLASSINE_HOME_WIDGETS", "/usr/share/glassine-home/widgets"));
	snprintf(h->system_root, sizeof h->system_root, "%s",
		 env_or("GLASSINE_HOME_SYSTEM_WIDGETS", "/usr/share/glassine/widgets"));
	snprintf(h->native_root, sizeof h->native_root, "%s/app-category",
		 env_or("GLASSINE_HOME_NATIVE", "/usr/share/glassine-home/native"));
	char err[256];
	h->have_native_cat = manifest_load(h->native_root, &h->native_cat, err, sizeof err);
	if (!h->have_native_cat)
		fprintf(stderr, "glassine-home: App category's settings: %s\n", err);
	load_voice(h);
	load_dither(h);
	apps_load(&h->apps);
	catalog_scan(&h->cat, h->builtin_root, h->system_root, getenv("GLASSINE_HOME_USER_WIDGETS"));

	const char *cp = getenv("GLASSINE_HOME_CONF");
	if (cp && *cp)
		snprintf(h->conf_path, sizeof h->conf_path, "%s", cp);
	else if (!conf_default_path(h->conf_path, sizeof h->conf_path))
		snprintf(h->conf_path, sizeof h->conf_path, "/tmp/glassine-home.conf");
	/* one read of the file: what Home loads and what an outside change is compared with */
	enum conf_state cs;
	h->disk_text = conf_read(h->conf_path, &h->disk_len, &cs, &h->conf, err, sizeof err);
	bool fresh = cs == CONF_MISSING, loaded = cs == CONF_OK || fresh;
	if (!loaded) {
		/* broken, newer or unreadable: Home starts empty and leaves the file alone (FR-043); its
		 * changes are written when a readable file appears (conf_changed) */
		h->conf.read_only = true;
		fprintf(stderr, "glassine-home: %s; leaving it as it is\n", err[0] ? err : h->conf_path);
	}
	if (fresh || (loaded && !h->conf.has_widgets))
		first_layout(h);
	cards_sync(h);
	h->iq = imgq_start();
	watch_static(h);
	h->W = 702;
	h->H = 884;
	h->S = 1;
	return true;
}

void home_free(struct home *h)
{
	/* one last try for a change still waiting for the lock or a failed write (no waiting for
	 * the lock; if it's held or the write fails again, the change is lost with the process) */
	if (h->save_pending)
		save_conf(h);
	if (h->run) {
		run_free(h->run);
		run_result_free(&h->rr);
	}
	imgq_stop(h->iq);
	for (size_t i = 0; i < h->ncards; i++)
		card_free(&h->cards[i]);
	free(h->cards);
	catalog_free(&h->cat);
	apps_free(&h->apps);
	conf_free(&h->conf);
	free(h->disk_text);
	if (h->have_native_cat)
		manifest_free(&h->native_cat);
	for (int i = 0; i < h->nwatch; i++)
		free(h->watch[i].dir);
	if (h->ino >= 0)
		close(h->ino);
	if (h->cur)
		cairo_surface_destroy(h->cur);
}

void home_resize(struct home *h, int W, int H, int S)
{
	if (W == h->W && H == h->H && S == h->S && h->cur)
		return;
	h->W = W;
	h->H = H;
	h->S = S < 1 ? 1 : S;
	h->orient = W > H ? ORIENT_LANDSCAPE : ORIENT_PORTRAIT;
	if (h->cur)
		cairo_surface_destroy(h->cur);
	h->cur = NULL; /* the next paint damages everything */
	h->focus = -1;
	for (size_t i = 0; i < h->ncards; i++)
		for (int k = 0; k < DESC_MAX_IMAGES; k++)
			img_clear(&h->cards[i].img[k]); /* slot sizes may follow the card */
	h->dirty_now = true;
}

void home_appear(struct home *h)
{
	for (size_t i = 0; i < h->ncards; i++)
		if (h->cards[i].m && h->cards[i].nat == NAT_NONE) {
			h->cards[i].want_run = true;
			h->cards[i].trig = TRIGGER_APPEAR;
		}
}

/* ---- widget runs --------------------------------------------------------------- */

static void start_next_run(struct home *h)
{
	if (h->run || !h->z)
		return;
	int pick = -1;
	bool edit = false;
	/* the open settings sheet first, then cards in order */
	if (h->sheet == SHEET_SETTINGS && h->sheet_card >= 0 && h->cards[h->sheet_card].want_edit) {
		pick = h->sheet_card;
		edit = true;
	}
	for (size_t i = 0; pick < 0 && i < h->ncards; i++) {
		struct card *c = &h->cards[i];
		if (c->want_run) {
			if (c->nat != NAT_NONE || !c->m || !state_should_run(&c->st, c->trig)) {
				c->want_run = false;
				continue;
			}
			pick = (int)i;
		}
	}
	if (pick < 0)
		return;
	struct card *c = &h->cards[pick];
	if (edit)
		c->want_edit = false;
	else
		c->want_run = false;
	struct value *apps = c->nat == NAT_CAT ? apps_ctx(&h->apps) : NULL;
	struct run_opts o = { .edit = edit, .now = home_now(h), .apps = apps };
	h->run = run_start(h->z, c->dir, c->m, c->props, &o, &h->rr);
	v_free(apps);
	h->run_card = pick;
	h->run_edit = edit;
	if (!h->run) {
		run_result_free(&h->rr);
		return;
	}
}


static void run_finished(struct home *h)
{
	struct run_result *r = &h->rr;
	if (h->run_card < 0 || (size_t)h->run_card >= h->ncards) {
		run_result_free(r);
		return;
	}
	struct card *c = &h->cards[h->run_card];
	if (h->run_edit) {
		if (r->status == RUN_OK) {
			v_free(c->edit_desc);
			c->edit_desc = r->desc;
			r->desc = NULL;
		} else {
			snprintf(c->fail, sizeof c->fail, "%s", r->msg);
		}
		h->dirty_now = true;
		run_result_free(r);
		return;
	}
	enum outcome out = state_record(&c->st, r->status, r->hash);
	if (r->status == RUN_OK) {
		c->fail[0] = 0;
		for (size_t i = 0; i < c->nfiles; i++)
			free(c->files[i]);
		for (size_t i = 0; i < r->nfiles; i++) {
			c->files[i] = r->files[i];
			r->files[i] = NULL;
		}
		c->nfiles = r->nfiles;
		r->nfiles = 0;
		rewatch(h);
	} else {
		snprintf(c->fail, sizeof c->fail, "%s", r->msg);
		fprintf(stderr, "glassine-home: %s: %s: %s\n", c->ce ? c->ce->ref : "?", run_status_name(r->status), r->msg);
	}
	if (out == OUTCOME_CHANGED) {
		v_free(c->desc);
		c->desc = r->desc;
		r->desc = NULL;
	}
	if (out != OUTCOME_UNCHANGED)
		h->dirty_bg = true;
	run_result_free(r);
}

/* Image slots are requested by the scene when it lays a card out (the slot's size
 * follows the card), and a changed output always paints within a second. */

/* ---- commands ------------------------------------------------------------------- */

/* Runs CMD's words plus args, detached (a double fork: no child to reap). */
static void spawn_cmd(const char *cmd, const char *const *args, int nargs)
{
	char buf[512];
	snprintf(buf, sizeof buf, "%s", cmd);
	char *argv[40];
	int n = 0;
	char *sv = NULL;
	for (char *t = strtok_r(buf, " ", &sv); t && n < 30; t = strtok_r(NULL, " ", &sv))
		argv[n++] = t;
	for (int i = 0; i < nargs && n < 39; i++)
		argv[n++] = (char *)args[i];
	argv[n] = NULL;
	if (!n)
		return;
	pid_t p = fork();
	if (p == 0) {
		if (fork() == 0) {
			setsid();
			execvp(argv[0], argv);
			_exit(127);
		}
		_exit(0);
	}
	if (p > 0)
		waitpid(p, NULL, 0);
}

static void launch(struct home *h, const char *id, const char *where, const char *file)
{
	const char *args[3] = { id, where, file };
	apps_mark_opened(&h->apps, id, home_now(h));
	spawn_cmd(h->launch_cmd, args, file ? 3 : 2);
}

static void hide(struct home *h)
{
	spawn_cmd(h->hide_cmd, NULL, 0);
}

/* A widget's action: open an app (with a file), or a Settings page. */
static void do_action(struct home *h, struct card *c, const struct value *a);

/* ---- layout helpers ------------------------------------------------------------- */

static int *sizes_of(const struct home *h)
{
	int *s = malloc((h->conf.n + 1) * sizeof *s);
	for (size_t i = 0; s && i < h->conf.n; i++)
		s[i] = h->conf.w[i].size;
	return s;
}

static int page_of(const struct home *h, size_t idx)
{
	int *s = sizes_of(h);
	struct pack_pos *pos = calloc(h->conf.n + 1, sizeof *pos);
	int p = 0;
	if (s && pos) {
		pack(h->orient, s, h->conf.n, pos);
		p = idx < h->conf.n ? pos[idx].page : 0;
	}
	free(s);
	free(pos);
	return p;
}

static int npages(const struct home *h)
{
	int *s = sizes_of(h);
	struct pack_pos *pos = calloc(h->conf.n + 1, sizeof *pos);
	int n = s && pos ? pack(h->orient, s, h->conf.n, pos) : 0;
	free(s);
	free(pos);
	return n;
}

static void clamp_page(struct home *h)
{
	int n = npages(h);
	if (h->edit) {
		/* Arrange: the Add widget card may open a page of its own */
		int *s = sizes_of(h);
		struct pack_pos *pos = calloc(h->conf.n + 2, sizeof *pos);
		if (s && pos) {
			s[h->conf.n] = SIZE_HALF;
			n = pack(h->orient, s, h->conf.n + 1, pos);
		}
		free(s);
		free(pos);
	}
	if (n < 1)
		n = 1;
	if (h->page >= n)
		h->page = n - 1;
	if (h->page < 0)
		h->page = 0;
}

/* ---- settings ------------------------------------------------------------------- */

bool card_has_settings(const struct home *h, const struct card *c)
{
	return c->m && c->m->has_edit;
}

static void open_settings(struct home *h, int ci)
{
	struct card *c = &h->cards[ci];
	if (!card_has_settings(h, c))
		return;
	h->sheet = SHEET_SETTINGS;
	h->sheet_card = ci;
	h->sheet_page = 0;
	c->want_edit = true;
	h->focus = -1;
}

static void apply_set(struct home *h, int ci, const struct value *set)
{
	struct card *c = &h->cards[ci];
	struct instance *in = &h->conf.w[ci];
	char err[256];
	if (!c->m || !set || set->t != V_MAP)
		return;
	struct value *saved = in->props ? v_dup(in->props) : v_map();
	struct value *eff = props_effective(c->m, saved);
	if (!props_apply(c->m, eff, set, err, sizeof err)) {
		fprintf(stderr, "glassine-home: settings: %s\n", err);
		v_free(saved);
		v_free(eff);
		return;
	}
	for (size_t i = 0; i < set->u.m.n; i++)
		v_map_set(saved, set->u.m.k[i], v_dup(v_map_get(eff, set->u.m.k[i])));
	v_free(eff);
	v_free(in->props);
	in->props = saved;
	card_props(h, c, in);
	c->want_run = true;
	c->trig = TRIGGER_SETTINGS;
	c->want_edit = true;
	c->apage = 0;
	h->dirty_now = true;
	save_conf(h); /* last: taking in an outside change may move cards */
}

/* ---- the gallery ---------------------------------------------------------------- */

static void add_widget(struct home *h, int row)
{
	struct value *props = NULL;
	int size = SIZE_WIDE, idx;
	const char *ref;
	if (row < 3) {
		static const char *const refs[3] = { "home/favourites", "home/all-apps", "home/app-category" };
		static const int sizes[3] = { 2, 3, 2 };
		ref = refs[row];
		size = sizes[row];
		if (row == 2) {
			props = v_map();
			for (const char *const *k = apps_categories(); *k; k++)
				if (apps_count_in(&h->apps, *k)) {
					v_map_set(props, "category", v_str(*k));
					break;
				}
		}
	} else {
		size_t e = (size_t)(row - 3);
		if (e >= h->cat.n || !h->cat.e[e].ok)
			return;
		ref = h->cat.e[e].ref;
		size = h->cat.e[e].m.sizes[0];
	}
	idx = conf_add(&h->conf, ref, size, props);
	v_free(props);
	if (idx < 0)
		return;
	char uuid[37];
	memcpy(uuid, h->conf.w[idx].uuid, sizeof uuid);
	save_conf(h);
	cards_sync(h);
	idx = conf_find(&h->conf, uuid);
	if (idx < 0)
		return;
	h->sheet = SHEET_NONE;
	h->page = page_of(h, (size_t)idx);
	if (card_has_settings(h, &h->cards[idx]))
		open_settings(h, idx);
}

/* ---- actions ---------------------------------------------------------------------- */

static bool is_fav(const struct home *h, const char *id)
{
	for (size_t i = 0; i < h->conf.nfav; i++)
		if (!strcmp(h->conf.favourites[i], id))
			return true;
	return false;
}

static void toggle_fav(struct home *h, const char *id)
{
	for (size_t i = 0; i < h->conf.nfav; i++)
		if (!strcmp(h->conf.favourites[i], id)) {
			free(h->conf.favourites[i]);
			memmove(&h->conf.favourites[i], &h->conf.favourites[i + 1],
				(h->conf.nfav - i - 1) * sizeof *h->conf.favourites);
			h->conf.nfav--;
			save_conf(h);
			return;
		}
	char **nf = realloc(h->conf.favourites, (h->conf.nfav + 1) * sizeof *nf);
	if (!nf)
		return;
	h->conf.favourites = nf;
	h->conf.favourites[h->conf.nfav++] = strdup(id);
	save_conf(h);
}

bool home_helper_gone(const struct home *h)
{
	return h->z && h->z->gone;
}

bool home_is_fav(const struct home *h, const char *id)
{
	return is_fav(h, id);
}

static void do_action(struct home *h, struct card *c, const struct value *a)
{
	const struct value *k = a ? v_map_get(a, "a") : NULL;
	if (!k || k->t != V_STR)
		return;
	if (!strcmp(k->u.s.p, "open")) {
		const struct value *app = v_map_get(a, "app"), *file = v_map_get(a, "file");
		const struct app *ap = app ? apps_resolve(&h->apps, app->u.s.p) : NULL;
		launch(h, ap ? ap->id : app->u.s.p, "new", file ? file->u.s.p : NULL);
	} else if (!strcmp(k->u.s.p, "settings")) {
		const struct value *pg = v_map_get(a, "page");
		launch(h, "glassine-settings", "new", pg ? pg->u.s.p : NULL);
	} else if (!strcmp(k->u.s.p, "set")) {
		apply_set(h, (int)(c - h->cards), v_map_get(a, "props"));
	}
}

static const struct value *desc_item(const struct value *d, int i)
{
	return d && d->t == V_ARR && i >= 0 && (size_t)i < d->u.a.n ? d->u.a.v[i] : NULL;
}

static void cycle_size(struct home *h, int ci)
{
	struct instance *in = &h->conf.w[ci];
	struct card *c = &h->cards[ci];
	if (c->nat != NAT_NONE) {
		static const int rows[] = { 1, 2, 3, 5 };
		int n = h->orient == ORIENT_LANDSCAPE ? 3 : 4, k = 0;
		for (int i = 0; i < 4; i++)
			if (rows[i] == in->size)
				k = i;
		in->size = rows[(k + 1) % n];
	} else if (c->m && c->m->nsizes > 1) {
		in->size = in->size == c->m->sizes[0] ? c->m->sizes[1] : c->m->sizes[0];
	} else {
		return;
	}
	c->apage = 0;
	save_conf(h);
	h->page = page_of(h, (size_t)ci);
}

static void activate(struct home *h, const struct hit *t)
{
	struct card *c = t->a >= 0 && (size_t)t->a < h->ncards ? &h->cards[t->a] : NULL;
	h->dirty_now = true;
	switch (t->act) {
	case A_NONE:
		break;
	case A_PAGE_PREV:
		if (h->page > 0)
			h->page--;
		break;
	case A_PAGE_NEXT:
		if (h->page + 1 < h->npages)
			h->page++;
		break;
	case A_ARRANGE:
		h->edit = true;
		h->focus = -1;
		break;
	case A_DONE:
		h->edit = false;
		h->sheet = SHEET_NONE;
		h->focus = -1;
		clamp_page(h);
		break;
	case A_APP:
		launch(h, t->id, "new", NULL);
		break;
	case A_APPS_PREV:
		if (c && c->apage > 0)
			c->apage--;
		break;
	case A_APPS_NEXT:
		if (c)
			c->apage++; /* scene.c clamps */
		break;
	case A_WBUTTON:
		if (c) {
			const struct value *it = desc_item(c->desc, t->b);
			do_action(h, c, it ? v_map_get(it, "action") : NULL);
		}
		break;
	case A_CTL_SETTINGS:
		if (c)
			open_settings(h, t->a);
		break;
	case A_CTL_SIZE:
		if (c)
			cycle_size(h, t->a);
		break;
	case A_CTL_REMOVE:
		if (c) {
			conf_remove(&h->conf, (size_t)t->a);
			save_conf(h);
			cards_sync(h);
			clamp_page(h);
		}
		break;
	case A_ADD_CARD:
		h->sheet = SHEET_GALLERY;
		h->sheet_page = 0;
		h->focus = -1;
		for (size_t i = 0; i < h->ncards; i++)
			if (h->cards[i].st.paused) {
				state_gallery_opened(&h->cards[i].st);
				h->cards[i].want_run = true;
				h->cards[i].trig = TRIGGER_APPEAR;
			}
		break;
	case A_SWAP: {
		size_t *order = malloc(h->conf.n * sizeof *order);
		if (!order)
			break;
		for (size_t i = 0; i < h->conf.n; i++)
			order[i] = i;
		order[t->a] = (size_t)t->b;
		order[t->b] = (size_t)t->a;
		conf_reorder(&h->conf, order);
		free(order);
		save_conf(h);
		cards_sync(h);
		break;
	}
	case A_PAGE_SWAP: {
		int *s = sizes_of(h);
		size_t *order = malloc((h->conf.n + 1) * sizeof *order);
		if (s && order && pack_swap_pages(h->orient, s, h->conf.n, t->a, t->b, order)) {
			conf_reorder(&h->conf, order);
			save_conf(h);
			cards_sync(h);
			h->page = t->b;
		}
		free(s);
		free(order);
		break;
	}
	case A_PEEK:
		h->page = t->a;
		break;
	case A_SHEET_CLOSE:
	case A_SHEET_OUTSIDE:
		if (h->sheet == SHEET_SETTINGS && h->sheet_card >= 0 && (size_t)h->sheet_card < h->ncards) {
			v_free(h->cards[h->sheet_card].edit_desc);
			h->cards[h->sheet_card].edit_desc = NULL;
		}
		h->sheet = SHEET_NONE;
		h->focus = -1;
		break;
	case A_SHEET_PREV:
		if (h->sheet_page > 0)
			h->sheet_page--;
		break;
	case A_SHEET_NEXT:
		if (h->sheet_page + 1 < h->sheet_pages)
			h->sheet_page++;
		break;
	case A_MENU_NEW:
	case A_MENU_HERE:
		h->sheet = SHEET_NONE;
		launch(h, h->menu_app, t->act == A_MENU_NEW ? "new" : "here", NULL);
		break;
	case A_MENU_FAV:
		h->sheet = SHEET_NONE;
		toggle_fav(h, h->menu_app);
		break;
	case A_MENU_CONF: {
		char page[APP_ID_MAX + 8];
		snprintf(page, sizeof page, "apps/%s", h->menu_app);
		h->sheet = SHEET_NONE;
		launch(h, "glassine-settings", "new", page);
		break;
	}
	case A_GAL_ADD:
		add_widget(h, t->a);
		break;
	case A_SET_ROW:
	case A_SET_BUTTON:
		if (h->sheet_card >= 0 && (size_t)h->sheet_card < h->ncards) {
			struct card *sc = &h->cards[h->sheet_card];
			const struct value *it = desc_item(sc->edit_desc, t->a), *act = NULL;
			if (it && t->act == A_SET_ROW) {
				const struct value *rows = v_map_get(it, "rows");
				const struct value *row = rows && t->b >= 0 && (size_t)t->b < rows->u.a.n ? rows->u.a.v[t->b] : NULL;
				act = row ? v_map_get(row, "action") : NULL;
			} else if (it) {
				act = v_map_get(it, "action");
			}
			do_action(h, sc, act);
		}
		break;
	}
}

/* ---- input ---------------------------------------------------------------------- */

static bool in_rect(const struct rect *r, double x, double y)
{
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* how far (x, y) is from r, 0 inside */
static double rect_dist(const struct rect *r, double x, double y)
{
	double dx = x < r->x ? r->x - x : x >= r->x + r->w ? x - (r->x + r->w) : 0;
	double dy = y < r->y ? r->y - y : y >= r->y + r->h ? y - (r->y + r->h) : 0;
	return hypot(dx, dy);
}

/*
 * The hit a press at (x, y) is for. Above an open sheet's backdrop only (it is the
 * barrier: what lies under the sheet can't be pressed): a drawn box that holds the point,
 * the topmost first; else the nearest one whose touch area holds it, a box grown to
 * TOUCH_MIN around its centre (FR-041), so a gap between two controls goes to the closer;
 * else the backdrop. The drawn box (and the focus ring around it) stays as it is.
 */
static int hit_at(const struct home *h, double x, double y)
{
	int barrier = -1;
	for (int i = h->nhits - 1; i >= 0 && barrier < 0; i--)
		if (h->hits[i].act == A_SHEET_OUTSIDE)
			barrier = i;
	for (int i = h->nhits - 1; i > barrier; i--)
		if (in_rect(&h->hits[i].r, x, y))
			return i;
	int best = -1;
	double best_d = 0;
	for (int i = h->nhits - 1; i > barrier; i--) {
		const struct rect *r = &h->hits[i].r;
		double gw = r->w < TOUCH_MIN ? (TOUCH_MIN - r->w) / 2 : 0, gh = r->h < TOUCH_MIN ? (TOUCH_MIN - r->h) / 2 : 0;
		struct rect t = { r->x - gw, r->y - gh, r->w + 2 * gw, r->h + 2 * gh };
		double d = rect_dist(r, x, y);
		if (in_rect(&t, x, y) && (best < 0 || d < best_d)) {
			best = i;
			best_d = d;
		}
	}
	if (best >= 0)
		return best;
	return barrier >= 0 && in_rect(&h->hits[barrier].r, x, y) ? barrier : -1;
}

void home_press(struct home *h, double x, double y)
{
	h->pressing = true;
	h->held = false;
	h->press_hit = hit_at(h, x, y);
	h->press_ms = mono_ms();
	h->px = x;
	h->py = y;
}

static void hold_fire(struct home *h)
{
	const struct hit *t = &h->hits[h->press_hit];
	h->held = true;
	if (t->act != A_APP || h->edit)
		return;
	snprintf(h->menu_app, sizeof h->menu_app, "%s", t->id);
	h->sheet = SHEET_MENU;
	h->sheet_page = 0;
	h->focus = -1;
	h->dirty_now = true;
}

void home_release(struct home *h, double x, double y)
{
	if (!h->pressing)
		return;
	h->pressing = false;
	if (h->held || h->press_hit < 0)
		return;
	if (h->hits[h->press_hit].act == A_APP && !h->edit && mono_ms() - h->press_ms >= HOLD_MS) {
		hold_fire(h); /* the loop was late: still a hold */
		return;
	}
	if (hit_at(h, x, y) != h->press_hit)
		return;
	struct hit t = h->hits[h->press_hit];
	activate(h, &t);
}

void home_cancel(struct home *h)
{
	h->pressing = false;
}

static int focusables(const struct home *h, int *idx, int max)
{
	int n = 0;
	for (int i = 0; i < h->nhits && n < max; i++)
		if (h->hits[i].focus)
			idx[n++] = i;
	return n;
}

void home_key(struct home *h, enum key k)
{
	int idx[MAX_HITS], n = focusables(h, idx, MAX_HITS);
	h->dirty_now = true;
	switch (k) {
	case K_TAB:
	case K_BACKTAB:
		if (!n)
			break;
		if (k == K_TAB)
			h->focus = h->focus < 0 ? 0 : (h->focus + 1) % n;
		else
			h->focus = h->focus <= 0 ? n - 1 : h->focus - 1;
		break;
	case K_ENTER:
	case K_SPACE:
		if (h->focus >= 0 && h->focus < n) {
			struct hit t = h->hits[idx[h->focus]];
			int keep = h->focus;
			activate(h, &t);
			if (h->sheet == SHEET_NONE && t.act != A_ARRANGE && t.act != A_DONE)
				h->focus = keep; /* the ring stays where it was */
		}
		break;
	case K_ESCAPE:
		if (h->sheet != SHEET_NONE) {
			struct hit t = { .act = A_SHEET_CLOSE, .a = -1 };
			activate(h, &t);
		} else if (h->edit) {
			struct hit t = { .act = A_DONE, .a = -1 };
			activate(h, &t);
		} else {
			hide(h);
			h->dirty_now = false;
		}
		break;
	}
}

/* ---- painting and the diff -------------------------------------------------------- */

static int region_for(const struct home *h, double cx, double cy)
{
	int best = -1;
	double area = 1e18;
	/* a sheet lies over the page: what changes inside it is the sheet's, not a card's beneath */
	for (int i = 0; i < h->nregions; i++) {
		const struct rect *r = &h->regions[i].r;
		if (h->regions[i].kind == R_SHEET && cx >= r->x && cx < r->x + r->w && cy >= r->y && cy < r->y + r->h)
			return i;
	}
	for (int i = 0; i < h->nregions; i++) {
		const struct rect *r = &h->regions[i].r;
		if (cx >= r->x && cx < r->x + r->w && cy >= r->y && cy < r->y + r->h && r->w * r->h < area) {
			area = r->w * r->h;
			best = i;
		}
	}
	return best;
}

static void grow(struct rect *a, const struct rect *b)
{
	if (a->w <= 0) {
		*a = *b;
		return;
	}
	double x0 = a->x < b->x ? a->x : b->x, y0 = a->y < b->y ? a->y : b->y;
	double x1 = a->x + a->w > b->x + b->w ? a->x + a->w : b->x + b->w;
	double y1 = a->y + a->h > b->y + b->h ? a->y + a->h : b->y + b->h;
	*a = (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

#define TILE 16

void home_paint(struct home *h, bool bg)
{
	if (bg && h->dirty_bg)
		h->dirty_now = true;
	if (!h->dirty_now && h->cur)
		return;
	h->dirty_now = false;
	if (bg)
		h->dirty_bg = false;
	clamp_page(h);
	int dw = h->W * h->S, dh = h->H * h->S;
	cairo_surface_t *next = cairo_image_surface_create(CAIRO_FORMAT_RGB24, dw, dh);
	cairo_t *cr = cairo_create(next);
	cairo_scale(cr, h->S, h->S);
	scene_paint(h, cr);
	cairo_destroy(cr);
	cairo_surface_flush(next);
	h->minute = (long)(home_now(h) / 60);

	struct rect dmg[MAX_DAMAGE];
	int nd = 0;
	if (!h->cur) {
		dmg[nd++] = (struct rect){ 0, 0, dw, dh };
	} else {
		struct rect per[MAX_REGIONS];
		memset(per, 0, sizeof per);
		const unsigned char *a = cairo_image_surface_get_data(h->cur), *b = cairo_image_surface_get_data(next);
		int stride = cairo_image_surface_get_stride(next);
		bool overflow = false;
		for (int ty = 0; ty < dh; ty += TILE)
			for (int tx = 0; tx < dw; tx += TILE) {
				int tw = tx + TILE > dw ? dw - tx : TILE, th = ty + TILE > dh ? dh - ty : TILE;
				bool diff = false;
				for (int y = ty; y < ty + th && !diff; y++)
					diff = memcmp(a + (size_t)y * stride + tx * 4, b + (size_t)y * stride + tx * 4, (size_t)tw * 4) != 0;
				if (!diff)
					continue;
				struct rect tr = { tx, ty, tw, th };
				int rg = region_for(h, (tx + tw / 2.0) / h->S, (ty + th / 2.0) / h->S);
				if (rg >= 0) {
					const struct rect *r = &h->regions[rg].r;
					struct rect dr = { r->x * h->S, r->y * h->S, r->w * h->S, r->h * h->S };
					grow(&per[rg], &dr);
					grow(&per[rg], &tr);
				} else if (nd < MAX_DAMAGE) {
					dmg[nd++] = tr;
				} else {
					overflow = true;
				}
			}
		for (int i = 0; i < h->nregions; i++)
			if (per[i].w > 0) {
				if (nd < MAX_DAMAGE)
					dmg[nd++] = per[i];
				else
					overflow = true;
			}
		if (overflow) {
			nd = 1;
			dmg[0] = (struct rect){ 0, 0, dw, dh };
		}
		/* clamp to the canvas, whole pixels */
		for (int i = 0; i < nd; i++) {
			double x0 = dmg[i].x < 0 ? 0 : (int)dmg[i].x, y0 = dmg[i].y < 0 ? 0 : (int)dmg[i].y;
			double x1 = dmg[i].x + dmg[i].w, y1 = dmg[i].y + dmg[i].h;
			x1 = x1 > dw ? dw : (int)(x1 + 0.999);
			y1 = y1 > dh ? dh : (int)(y1 + 0.999);
			dmg[i] = (struct rect){ x0, y0, x1 - x0, y1 - y0 };
		}
	}
	if (h->cur)
		cairo_surface_destroy(h->cur);
	h->cur = next;
	if (nd) {
		h->commits++;
		if (h->io.commit)
			h->io.commit(h->io.ud, dmg, nd);
	}
}

/* ---- the event loop --------------------------------------------------------------- */

int home_fds(struct home *h, int *fds, int max)
{
	int n = 0;
	if (h->run && run_fd(h->run) >= 0 && n < max)
		fds[n++] = run_fd(h->run);
	if (h->ino >= 0 && n < max)
		fds[n++] = h->ino;
	if (h->iq && imgq_fd(h->iq) >= 0 && n < max)
		fds[n++] = imgq_fd(h->iq);
	return n;
}

static void inotify_event(struct home *h, int wd, uint32_t mask, const char *name)
{
	long long now = mono_ms();
	if (wd == h->wd_conf && wd >= 0 && (mask & IN_IGNORED)) {
		/* the config folder was removed: its watch is gone; the next write makes the folder and watches it again */
		h->wd_conf = -1;
		return;
	}
	if (wd == h->wd_conf && wd >= 0 && name) {
		const char *base = strrchr(h->conf_path, '/');
		if (!strcmp(name, base ? base + 1 : h->conf_path)) {
			if (mask & (IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM))
				conf_changed(h);
			return;
		}
	}
	for (int i = 0; i < 16; i++)
		if (wd == h->wd_apps[i] && wd >= 0) {
			apps_load(&h->apps);
			h->dirty_bg = true;
			for (size_t k = 0; k < h->ncards; k++)
				if (h->cards[k].nat == NAT_CAT && h->sheet == SHEET_SETTINGS && h->sheet_card == (int)k)
					h->cards[k].want_edit = true;
			return;
		}
	for (int i = 0; i < 3; i++)
		if (wd == h->wd_cat[i] && wd >= 0) {
			rescan(h);
			h->dirty_bg = true;
			return;
		}
	if (wd == h->wd_shellconf && wd >= 0) {
		if (name && !strcmp(name, "theme")) {
			load_voice(h);
			h->dirty_bg = true;
		}
		return;
	}
	if (wd == h->wd_eink && wd >= 0) {
		const char *e = env_or("GLASSINE_HOME_EINK_CONF", "/etc/glassine/eink.conf"), *base = strrchr(e, '/');
		if (name && !strcmp(name, base ? base + 1 : e)) {
			bool d = h->dither;
			char pat[16];
			snprintf(pat, sizeof pat, "%s", h->dither_pattern);
			load_dither(h);
			if (d != h->dither || strcmp(pat, h->dither_pattern))
				for (size_t k = 0; k < h->ncards; k++) {
					for (int m = 0; m < DESC_MAX_IMAGES; m++)
						img_clear(&h->cards[k].img[m]);
					h->dirty_bg = true;
				}
		}
		return;
	}
	for (int i = 0; i < h->nwatch; i++) {
		if (h->watch[i].wd != wd)
			continue;
		for (size_t k = 0; k < h->ncards; k++) {
			struct card *c = &h->cards[k];
			if (c->m && c->m->on_change && name && watch_match(c->files, c->nfiles, h->watch[i].dir, name))
				debounce_event(&c->db, now, DEBOUNCE_QUIET_MS, DEBOUNCE_MAX_MS);
		}
	}
}

void home_ready(struct home *h, int fd)
{
	if (h->run && fd == run_fd(h->run)) {
		if (run_step(h->run)) {
			run_free(h->run);
			h->run = NULL;
			run_finished(h);
			start_next_run(h);
		}
		return;
	}
	if (fd == h->ino) {
		char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
		ssize_t n;
		while ((n = read(h->ino, buf, sizeof buf)) > 0)
			for (char *p = buf; p < buf + n;) {
				struct inotify_event *e = (struct inotify_event *)p;
				inotify_event(h, e->wd, e->mask, e->len ? e->name : NULL);
				p += sizeof *e + e->len;
			}
		return;
	}
	if (h->iq && fd == imgq_fd(h->iq)) {
		if (imgq_collect(h))
			h->dirty_bg = true;
	}
}

int home_tick(struct home *h)
{
	long long now = mono_ms();
	int wait = 60000;
	if (h->run) {
		if (run_timeout_ms(h->run) == 0 && run_step(h->run)) {
			run_free(h->run);
			h->run = NULL;
			run_finished(h);
		}
	}
	/* a hold */
	if (h->pressing && !h->held && h->press_hit >= 0 && h->hits[h->press_hit].act == A_APP && !h->edit) {
		long long due = h->press_ms + HOLD_MS;
		if (now >= due)
			hold_fire(h);
		else if (due - now < wait)
			wait = (int)(due - now);
	}
	/* debounced file changes and intervals */
	long long wall = home_now(h);
	for (size_t i = 0; i < h->ncards; i++) {
		struct card *c = &h->cards[i];
		if (debounce_fire(&c->db, now)) {
			c->want_run = true;
			c->trig = TRIGGER_CHANGE;
		} else if (c->db.pending && c->db.due_ms - now < wait) {
			wait = (int)(c->db.due_ms - now);
		}
		if (c->m && c->m->every_s && c->st.next_due) {
			if (wall >= c->st.next_due) {
				c->want_run = true;
				c->trig = TRIGGER_INTERVAL;
				c->st.next_due = sched_next(wall, c->m->every_s, tz_offset(wall));
			}
			long long left = (c->st.next_due - wall) * 1000;
			if (left < wait)
				wait = (int)(left < 0 ? 0 : left);
		}
	}
	if (h->save_pending) {
		if (now >= h->save_retry_ms)
			save_conf(h);
		if (h->save_pending) {
			long long due = h->save_retry_ms > now ? h->save_retry_ms - now : 100;
			if (wait > due)
				wait = (int)(due > 100 ? 100 : due);
		}
	}
	start_next_run(h);
	if (h->run) {
		int t = run_timeout_ms(h->run);
		if (t < wait)
			wait = t;
	}
	/* the clock in the header */
	if ((long)(wall / 60) != h->minute)
		h->dirty_bg = true;
	/* paint: input now (with whatever else changed), the rest at most once a second */
	if (h->dirty_now) {
		if (h->dirty_bg)
			h->last_bg_ms = now;
		home_paint(h, true);
	}
	if (h->dirty_bg) {
		long long due = h->last_bg_ms + BG_PAINT_MS;
		if (now >= due) {
			home_paint(h, true);
			h->last_bg_ms = now;
		} else if (due - now < wait) {
			wait = (int)(due - now);
		}
	}
	long long to_minute = (60 - wall % 60) * 1000;
	if (to_minute < wait)
		wait = (int)to_minute;
	return wait < 0 ? 0 : wait;
}
