/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The widget manifest: widget.toml beside widget.lua (and widget.edit.lua when the
 * widget has settings). It is data only; Home never runs code to list, describe or
 * add a widget. Format version 1, see docs/widgets.md.
 */
#ifndef GH_MANIFEST_H
#define GH_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"

#define WIDGET_FORMAT 1
#define MAX_READS 16
#define MAX_PATH_LEN 1024
#define MIN_EVERY_S (15 * 60)

struct read_entry {
	char *path;  /* as written: absolute or starting with "~/" */
	bool dir;    /* written with a trailing '/': a directory prefix */
};

struct manifest {
	int format;
	char *name, *icon, *description;
	int sizes[2];          /* SIZE_HALF / SIZE_WIDE; sizes[0] is the default */
	size_t nsizes;
	struct read_entry reads[MAX_READS];
	size_t nreads;
	bool on_change;        /* run when a file read in the last run changes */
	long every_s;          /* interval in seconds, 0 for none */
	struct value *props;   /* map of setting -> default (string, int, float or bool) */
	bool has_edit;         /* widget.edit.lua exists (set by manifest_load) */
};

/* Parses widget.toml text. Returns false with a message ("line N: ..." or "key: ...") in err. */
bool manifest_parse(const char *text, size_t len, struct manifest *m, char *err, size_t errlen);

/* Reads DIR/widget.toml (at most 64 KiB) and checks that DIR/widget.lua exists. */
bool manifest_load(const char *dir, struct manifest *m, char *err, size_t errlen);

void manifest_free(struct manifest *m);

/* "15m", "1h", "2h30m", "1d": seconds, or -1. */
long parse_duration(const char *s);

/* A folder name usable as a widget id: [a-z0-9][a-z0-9._-]{0,63}. */
bool widget_id_valid(const char *id);

/*
 * The settings a run gets: the manifest's defaults overlaid with saved values of the
 * same type (an integer is accepted for a float setting). Unknown keys are dropped.
 */
struct value *props_effective(const struct manifest *m, const struct value *saved);

/*
 * Applies a ui.set{...} from a settings script to props (in place). Every key must be a
 * declared setting and the value must have its type. Returns false with err on a mismatch,
 * leaving props unchanged.
 */
bool props_apply(const struct manifest *m, struct value *props, const struct value *set, char *err, size_t errlen);

#endif
