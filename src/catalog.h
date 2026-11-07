/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The widgets Home can offer: one folder per widget under three roots, read from
 * widget.toml only (no code runs to list, describe or add a widget).
 *
 *   builtin  shipped with glassine-home
 *   system   brought by apps, /usr/share/glassine/widgets
 *   user     the user's own, ~/.local/share/glassine/widgets ($XDG_DATA_HOME respected)
 *
 * A folder with a broken manifest stays in the list with its error, so the gallery can
 * say what is wrong. Home watches the roots (catalog_roots) and rescans on a change,
 * which picks up a new folder without a restart.
 */
#ifndef GH_CATALOG_H
#define GH_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

#include "manifest.h"

#define CATALOG_MAX 256

struct catalog_entry {
	char ref[80];        /* "source/id", as home.conf stores it */
	char *dir;
	bool ok;
	struct manifest m;   /* valid when ok */
	char err[256];       /* when not ok */
};

struct catalog {
	struct catalog_entry *e;
	size_t n;
	char *roots[3];      /* builtin, system, user; NULL when not set */
};

/* Any root may be NULL. The user root defaults to $XDG_DATA_HOME or ~/.local/share + /glassine/widgets. */
void catalog_scan(struct catalog *c, const char *builtin_root, const char *system_root, const char *user_root);
void catalog_free(struct catalog *c);
const struct catalog_entry *catalog_find(const struct catalog *c, const char *ref);
bool catalog_user_root(char *out, size_t len);

#endif
