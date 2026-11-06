/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Home's layout file, ~/.config/glassine/home.conf ($XDG_CONFIG_HOME respected):
 * the widgets in order, each with its instance id, widget, size and settings, plus
 * the favourite apps. Pages are never stored; they follow from packing.
 *
 *   version = 1
 *   favourites = ["org.example.App"]
 *
 *   [[widget]]
 *   uuid = "0f6d3c1e-...-..."
 *   widget = "builtin/reading"     # source/id: builtin, system, user, or home (native)
 *   size = "wide"                  # "half", "wide", or rows 1, 2, 3, 5 for app widgets
 *   props = { category = "Games" }
 *
 * Written to a temporary file in the same directory, synced, then renamed over.
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
};

/* $XDG_CONFIG_HOME/glassine/home.conf or $HOME/.config/glassine/home.conf */
bool conf_default_path(char *out, size_t len);

/*
 * Loads the file. A missing file is an empty layout. A file that can't be read is
 * renamed to home.conf.broken (kept, never deleted) and Home starts empty; err says so.
 * A newer version loads nothing and marks the layout read-only.
 */
bool conf_load(const char *path, struct home_conf *c, char *err, size_t errlen);
bool conf_save(const char *path, const struct home_conf *c, char *err, size_t errlen);
void conf_free(struct home_conf *c);

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
