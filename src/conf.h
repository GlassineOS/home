/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Home's layout file, ~/.config/glassine/home.conf ($XDG_CONFIG_HOME respected):
 * the widgets in order, each with its instance id, widget, size and settings, plus
 * the favourite apps. Pages are never stored; they follow from packing.
 *
 *   version = 1
 *   favourites = ["org.example.App"]
 *
 *   [[widget]]          # or widget = [] for a layout emptied on purpose
 *   uuid = "0f6d3c1e-...-..."
 *   widget = "builtin/reading"     # source/id: builtin, system, user, or home (native)
 *   size = "wide"                  # "half", "wide", or rows 1, 2, 3, 5 for app widgets
 *   props = { category = "Games" }
 *
 * The format and the rules for sharing the file (Settings edits favourites) are in
 * docs/home-conf.md. Every write goes to home.conf.tmp in the same directory, is synced,
 * then renamed over the file, under an exclusive flock on home.conf.lock.
 */
#ifndef GH_CONF_H
#define GH_CONF_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"

#define CONF_VERSION 1
#define CONF_MAX_WIDGETS 256

struct instance {
	char uuid[37];
	char *widget;
	int size;            /* SIZE_HALF, SIZE_WIDE, or rows */
	struct value *props; /* map of saved settings */
};

struct home_conf {
	struct instance *w;
	size_t n;
	char **favourites;
	size_t nfav;
	bool read_only;      /* written by a newer Home, or unreadable: never overwrite it */
	bool has_widgets;    /* the file has a widget key ([[widget]] tables or widget = []) */
};

/* $XDG_CONFIG_HOME/glassine/home.conf or $HOME/.config/glassine/home.conf */
bool conf_default_path(char *out, size_t len);

/*
 * Loads the file. A missing file is an empty layout. A file that can't be read, or a newer
 * version, loads nothing and marks the layout read-only: the file is left where it is and
 * is not written over (FR-043); err says why.
 */
bool conf_load(const char *path, struct home_conf *c, char *err, size_t errlen);
/* Writes the file under the lock (waiting for it). */
bool conf_save(const char *path, const struct home_conf *c, char *err, size_t errlen);
void conf_free(struct home_conf *c);

/*
 * The file without side effects: its bytes (NULL when missing or unreadable), and when
 * they parse (CONF_OK) the layout in c. Nothing is renamed or locked; writers rename a
 * whole file into place, so a read needs no lock.
 */
enum conf_state { CONF_OK, CONF_MISSING, CONF_UNREADABLE, CONF_BROKEN, CONF_NEWER };
char *conf_read(const char *path, size_t *len, enum conf_state *state, struct home_conf *c, char *err, size_t errlen);
/* Parses a file's text; on failure c is empty (read_only set for a newer version). */
bool conf_parse(const char *text, size_t len, struct home_conf *c, char *err, size_t errlen);
/* The file's text as Home writes it; NULL (err) if a setting can't be written. */
char *conf_format(const struct home_conf *c, size_t *len, char *err, size_t errlen);
/*
 * The lock every writer takes: an exclusive flock on PATH.lock (created if missing,
 * never deleted). timeout_ms < 0 waits as long as it takes. Returns the fd to pass to
 * conf_unlock, or -1 (errno EWOULDBLOCK when the wait ran out).
 */
int conf_lock(const char *path, int timeout_ms);
void conf_unlock(int fd);
/* PATH.tmp, fsync, rename over PATH, fsync the directory. The caller holds the lock. */
bool conf_write_text(const char *path, const char *text, size_t len, char *err, size_t errlen);

bool conf_favourites_equal(const struct home_conf *a, const struct home_conf *b);
bool conf_widgets_equal(const struct home_conf *a, const struct home_conf *b);
bool conf_instance_equal(const struct instance *a, const struct instance *b);
/* Moves src's favourites (or widgets) into dst, freeing dst's. */
void conf_take_favourites(struct home_conf *dst, struct home_conf *src);
void conf_take_widgets(struct home_conf *dst, struct home_conf *src);

/* "builtin/reading", "user/my-clock", "home/all-apps" */
bool conf_widget_ref_valid(const char *ref);

/* Appends a widget (adding goes to the end); props may be NULL. Returns its index or -1. */
int conf_add(struct home_conf *c, const char *widget, int size, const struct value *props);
bool conf_remove(struct home_conf *c, size_t i);
bool conf_move(struct home_conf *c, size_t from, size_t to);
/* Reorders by a permutation (as pack_swap_pages gives). */
bool conf_reorder(struct home_conf *c, const size_t *order);
int conf_find(const struct home_conf *c, const char *uuid);

/* A random version 4 UUID. */
bool uuid_v4(char out[37]);

#endif
