/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A small value tree (null, bool, int, float, string, array, map) shared by the
 * manifest, the widget description, settings and the configuration file, with a
 * canonical JSON writer (same value = same bytes) and a strict JSON reader.
 */
#ifndef GH_VALUE_H
#define GH_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum vtype { V_NULL, V_BOOL, V_INT, V_FLOAT, V_STR, V_ARR, V_MAP };

struct value {
	enum vtype t;
	union {
		bool b;
		long long i;
		double f;
		struct { char *p; size_t len; } s;
		struct { struct value **v; size_t n, cap; } a;
		/* maps keep insertion order; the canonical writer sorts keys */
		struct { char **k; struct value **v; size_t n, cap; } m;
	} u;
};

/*
 * Allocation failure calls v_oom, when set, and then abort(). The sandboxed run sets it
 * to end the run as over the memory limit (abort() needs system calls its filter forbids).
 */
extern void (*v_oom)(void);

struct value *v_null(void);
struct value *v_bool(bool b);
struct value *v_int(long long i);
struct value *v_float(double f);
struct value *v_str(const char *s);
struct value *v_strn(const char *s, size_t len);
struct value *v_arr(void);
struct value *v_map(void);
void v_free(struct value *v);
struct value *v_dup(const struct value *v);
bool v_equal(const struct value *a, const struct value *b);

/* Append / set take ownership of the child. v_map_set replaces an existing key. */
void v_arr_push(struct value *a, struct value *child);
void v_map_set(struct value *m, const char *key, struct value *child);
struct value *v_map_get(const struct value *m, const char *key);
bool v_map_del(struct value *m, const char *key);

/* Growable byte buffer. */
struct buf { char *p; size_t len, cap; };
void buf_add(struct buf *b, const void *p, size_t n);
void buf_adds(struct buf *b, const char *s);
void buf_addf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_free(struct buf *b);

/*
 * Canonical JSON: no whitespace, map keys sorted bytewise, integers as decimal,
 * floats with "%.17g" (non-finite floats as null), strings with the minimal escapes.
 * The result is NUL-terminated; returns false only on out-of-memory.
 */
bool v_to_json(const struct value *v, struct buf *out);

/*
 * Strict JSON reader: RFC 8259 grammar, valid UTF-8 only, no duplicate keys,
 * nesting at most max_depth. On error returns NULL and writes a message to err.
 */
struct value *v_from_json(const char *s, size_t len, int max_depth, char *err, size_t errlen);

/* True when s[0..len) is valid UTF-8 without NUL bytes and overlong forms. */
bool utf8_valid(const char *s, size_t len);
/* Longest prefix of s[0..len) no longer than max bytes that ends on a code point boundary. */
size_t utf8_cut(const char *s, size_t len, size_t max);

/* 64-bit FNV-1a. */
uint64_t hash64(const void *p, size_t n);

#endif
