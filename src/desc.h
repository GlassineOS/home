/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The description a widget returns, built from seven primitives. The sandboxed run
 * normalizes it once for good error messages; Home normalizes the run's output again,
 * because the run is untrusted. The canonical JSON of the normalized value is what
 * Home compares: same bytes = nothing to redraw.
 *
 * Normalized form (an array, in the widget's order):
 *   {"t":"title","text":S}                       at most one
 *   {"t":"text","text":S[,"soft":true]}
 *   {"t":"progress","permille":0..1000[,"label":S]}
 *   {"t":"list","rows":[{"label":S[,"meta":S][,"selected":true][,"action":A]}]}  4 rows, more are cut
 *   {"t":"button","label":S[,"icon":S],"action":A[,"selected":true]}             at most two
 *   {"t":"meta","text":S[,"time":unix seconds]}  Home appends the relative time ("2 h ago")
 *   {"t":"image","src":S,"h":N[,"w":N][,"side":"left"|"right"],"fit":"cover"|"contain","alt":S}  at most two
 * Actions: {"a":"open","app":ID[,"file":PATH]}, {"a":"settings","page":PAGE},
 *          {"a":"set","props":{...}} (settings scripts only).
 */
#ifndef GH_DESC_H
#define GH_DESC_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"

#define DESC_MAX_ITEMS 12
#define DESC_MAX_ROWS 4
#define DESC_MAX_BUTTONS 2
#define DESC_MAX_IMAGES 2
#define DESC_MAX_JSON (64 * 1024)
#define IMAGE_MAX_PX 2000

/* Checks a path from an image src (file:) against the widget's reads; NULL skips the check. */
typedef bool (*desc_path_check)(void *ctx, const char *path);

/* Returns the normalized description or NULL with a message in err. */
struct value *desc_normalize(const struct value *in, bool edit, desc_path_check check, void *ctx,
			     char *err, size_t errlen);

#endif
