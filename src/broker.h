/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The parent side of a widget's file access. The sandboxed run never opens a path:
 * it asks Home, which checks the request against the manifest's reads, opens the file
 * beneath the read's directory without following any symlink, and passes the open fd.
 */
#ifndef GH_BROKER_H
#define GH_BROKER_H

#include <stdbool.h>
#include <stddef.h>

#include "manifest.h"
#include "places.h"

#define BROKER_MAX_FILE (2 * 1024 * 1024)
#define BROKER_MAX_FILES 64

struct broker {
	const struct manifest *m;
	char home[1024];        /* expands "~/" */
	struct places places;   /* expands "{books}/" */
	bool force_fallback;    /* tests: use the openat walk instead of openat2 */
	char *files[BROKER_MAX_FILES]; /* absolute paths the run asked for (allowed ones), for watching */
	size_t nfiles;
	int requests;
};

/*
 * home may be NULL: then $HOME is used. The places come from home's places file (or
 * $XDG_CONFIG_HOME's when home is NULL).
 */
void broker_init(struct broker *b, const struct manifest *m, const char *home);
void broker_free(struct broker *b);

/*
 * Expands "~/" and "{place}/" and checks the path lexically (absolute, no empty, "." or ".." parts).
 * Writes the absolute path to out. Returns false for a malformed path.
 */
bool broker_expand(const struct broker *b, const char *req, char *out, size_t outlen);

/* True if the absolute path is covered by a read (a listed file, or under a listed directory). */
bool broker_allowed(const struct broker *b, const char *abs);

/*
 * Opens a file for the run. Returns an fd (regular file, at most BROKER_MAX_FILE bytes,
 * size in *size) or -1 with errno: EACCES (not in the reads, or a symlink on the way),
 * ENOENT, EFBIG, EINVAL (not a regular file or a malformed path), EMFILE (too many requests).
 * Allowed paths are recorded in b->files even when opening fails, so a file that
 * appears later is still watched.
 */
int broker_open(struct broker *b, const char *req, long long *size);

/* Shared with the image source check of a description: a file: path inside the reads. */
bool broker_path_check(void *broker, const char *path);

#endif
