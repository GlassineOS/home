/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Places: named folders a widget's reads may start with, set by the user.
 * The one place so far is {books}, the user's books folder (default ~/Books).
 *
 * The setting lives in ~/.config/glassine/places ($XDG_CONFIG_HOME respected), in the
 * same TOML subset as widget.toml, one string per place:
 *
 *   books = "~/Library"
 *
 * Values are absolute or start with "~/". Names this Home doesn't know are ignored
 * (other places can join later). A missing file means the defaults. A broken file, an
 * empty value, or a value that is not an allowed path leaves the place unset, and
 * reads inside an unset place fail: Home never falls back to a wider folder.
 */
#ifndef GH_PLACES_H
#define GH_PLACES_H

#include <stdbool.h>
#include <stddef.h>

#include "manifest.h"

struct places {
	char books[MAX_PATH_LEN + 1100]; /* resolved absolute path; "" when unset */
	char why[160];                   /* why books is unset */
};

/* True for a known place name (without the braces). */
bool place_known(const char *name, size_t len);

/* home/.config/glassine/places when home is given, else $XDG_CONFIG_HOME/glassine/places or $HOME/.config/... */
bool places_path(const char *home, char *out, size_t len);

/* Loads the places file (path may be NULL: defaults). home expands "~/". */
void places_load(struct places *p, const char *path, const char *home);

/* The resolved folder of a place, or NULL when unset or unknown. */
const char *places_get(const struct places *p, const char *name, size_t len);

#endif
