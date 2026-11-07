/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The apps Home's app widgets show: the desktop files of the XDG application
 * directories ($XDG_DATA_HOME, then each of $XDG_DATA_DIRS; Flatpak's exports are
 * among them), Type=Application, not NoDisplay/Hidden, shown in the current desktop.
 * A desktop id found twice keeps the first (the user's own wins).
 *
 * Categories follow the shell's drawer: the freedesktop Categories map onto Reading,
 * Office, Education, Games, Multimedia, Internet and System.
 *
 * An app that exists twice (a Flatpak and a package with the same name) shows twice,
 * each with its origin (spec 001 FR-012); an action that names an app by id resolves
 * to the one opened last (apps_resolve).
 */
#ifndef GH_APPS_H
#define GH_APPS_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"

#define APP_ID_MAX 256

struct app {
	char id[APP_ID_MAX];   /* desktop id without ".desktop" */
	char name[128];
	char icon[256];
	const char *cat;       /* one of apps_categories() */
	char origin[16];       /* "Flatpak" or "" (a package) */
	bool twin;             /* another app has the same name: show the origin */
	long long opened;      /* when Home last opened it (s), 0 = never */
};

struct apps {
	struct app *a;
	size_t n;              /* sorted A–Z by name */
};

/* The application directories, most important first; returns how many. */
size_t apps_dirs(char dirs[][1024], size_t max);
void apps_load(struct apps *l);
void apps_free(struct apps *l);
const struct app *apps_find(const struct apps *l, const char *id);

/*
 * The app an action names: the exact desktop id, else the one whose id's last dotted
 * part matches case-insensitively ("koreader" finds rocks.koreader.KOReader); when
 * several match, the one opened last. NULL when none.
 */
const struct app *apps_resolve(const struct apps *l, const char *id);

/* The category names in A–Z order, NULL-terminated. */
const char *const *apps_categories(void);
const char *apps_category_of(const char *freedesktop_categories);
size_t apps_count_in(const struct apps *l, const char *cat);

/* ctx.apps for Home's own widgets' settings: { categories = { {name=, count=} } } */
struct value *apps_ctx(const struct apps *l);

/* When each app was opened last: $XDG_STATE_HOME/glassine-home/opened ("time id" lines). */
void apps_load_opened(struct apps *l);
void apps_mark_opened(struct apps *l, const char *id, long long now);

#endif
