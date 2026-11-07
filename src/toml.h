/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A strict reader for a small subset of TOML 1.0, enough for widget manifests and
 * Home's configuration file:
 *
 *   - comments (#), blank lines
 *   - key = value, bare keys [A-Za-z0-9_-]+ or "quoted" keys; no dotted keys
 *   - [table] and [[array-of-tables]] headers, one level deep (no dots)
 *   - values: basic strings "..." (escapes \b \t \n \f \r \" \\ \uXXXX \UXXXXXXXX),
 *     literal strings '...', decimal integers, decimal floats, true/false,
 *     arrays (may span lines, trailing comma allowed), inline tables { k = v, ... }
 *
 * Everything else (dates, multi-line strings, hex/octal/binary, inf/nan, dotted keys)
 * is rejected with a line number. Duplicate keys and redefined tables are errors.
 */
#ifndef GH_TOML_H
#define GH_TOML_H

#include <stddef.h>

#include "value.h"

/* Returns a map, or NULL with "line N: message" in err. */
struct value *toml_parse(const char *s, size_t len, char *err, size_t errlen);

/* Parses one TOML value (e.g. an inline table) on its own. */
struct value *toml_parse_value(const char *s, size_t len, char *err, size_t errlen);

/* Writers for the subset: a quoted key if needed, and a scalar/array/inline-table value. */
void toml_put_key(struct buf *b, const char *key);
/* Returns false for values the subset can't write (null, non-finite floats). */
bool toml_put_value(struct buf *b, const struct value *v);

#endif
