// SPDX-License-Identifier: GPL-2.0-or-later
#include "value.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		abort();
	return p;
}

static void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		abort();
	return p;
}

static struct value *v_new(enum vtype t)
{
	struct value *v = xmalloc(sizeof *v);
	memset(v, 0, sizeof *v);
	v->t = t;
	return v;
}

struct value *v_null(void) { return v_new(V_NULL); }
struct value *v_bool(bool b) { struct value *v = v_new(V_BOOL); v->u.b = b; return v; }
struct value *v_int(long long i) { struct value *v = v_new(V_INT); v->u.i = i; return v; }
struct value *v_float(double f) { struct value *v = v_new(V_FLOAT); v->u.f = f; return v; }
struct value *v_arr(void) { return v_new(V_ARR); }
struct value *v_map(void) { return v_new(V_MAP); }

struct value *v_strn(const char *s, size_t len)
{
	struct value *v = v_new(V_STR);
	v->u.s.p = xmalloc(len + 1);
	memcpy(v->u.s.p, s, len);
	v->u.s.p[len] = 0;
	v->u.s.len = len;
	return v;
}

struct value *v_str(const char *s) { return v_strn(s, strlen(s)); }

void v_free(struct value *v)
{
	if (!v)
		return;
	switch (v->t) {
	case V_STR:
		free(v->u.s.p);
		break;
	case V_ARR:
		for (size_t i = 0; i < v->u.a.n; i++)
			v_free(v->u.a.v[i]);
		free(v->u.a.v);
		break;
	case V_MAP:
		for (size_t i = 0; i < v->u.m.n; i++) {
			free(v->u.m.k[i]);
			v_free(v->u.m.v[i]);
		}
		free(v->u.m.k);
		free(v->u.m.v);
		break;
	default:
		break;
	}
	free(v);
}

struct value *v_dup(const struct value *v)
{
	switch (v->t) {
	case V_NULL: return v_null();
	case V_BOOL: return v_bool(v->u.b);
	case V_INT: return v_int(v->u.i);
	case V_FLOAT: return v_float(v->u.f);
	case V_STR: return v_strn(v->u.s.p, v->u.s.len);
	case V_ARR: {
		struct value *a = v_arr();
		for (size_t i = 0; i < v->u.a.n; i++)
			v_arr_push(a, v_dup(v->u.a.v[i]));
		return a;
	}
	case V_MAP: {
		struct value *m = v_map();
		for (size_t i = 0; i < v->u.m.n; i++)
			v_map_set(m, v->u.m.k[i], v_dup(v->u.m.v[i]));
		return m;
	}
	}
	return v_null();
}

bool v_equal(const struct value *a, const struct value *b)
{
	if (a->t != b->t)
		return false;
	switch (a->t) {
	case V_NULL: return true;
	case V_BOOL: return a->u.b == b->u.b;
	case V_INT: return a->u.i == b->u.i;
	case V_FLOAT: return a->u.f == b->u.f;
	case V_STR: return a->u.s.len == b->u.s.len && !memcmp(a->u.s.p, b->u.s.p, a->u.s.len);
	case V_ARR:
		if (a->u.a.n != b->u.a.n)
			return false;
		for (size_t i = 0; i < a->u.a.n; i++)
			if (!v_equal(a->u.a.v[i], b->u.a.v[i]))
				return false;
		return true;
	case V_MAP:
		if (a->u.m.n != b->u.m.n)
			return false;
		for (size_t i = 0; i < a->u.m.n; i++) {
			struct value *o = v_map_get(b, a->u.m.k[i]);
			if (!o || !v_equal(a->u.m.v[i], o))
				return false;
		}
		return true;
	}
	return false;
}

void v_arr_push(struct value *a, struct value *child)
{
	if (a->u.a.n == a->u.a.cap) {
		a->u.a.cap = a->u.a.cap ? a->u.a.cap * 2 : 4;
		a->u.a.v = xrealloc(a->u.a.v, a->u.a.cap * sizeof *a->u.a.v);
	}
	a->u.a.v[a->u.a.n++] = child;
}

struct value *v_map_get(const struct value *m, const char *key)
{
	if (!m || m->t != V_MAP)
		return NULL;
	for (size_t i = 0; i < m->u.m.n; i++)
		if (!strcmp(m->u.m.k[i], key))
			return m->u.m.v[i];
	return NULL;
}

void v_map_set(struct value *m, const char *key, struct value *child)
{
	for (size_t i = 0; i < m->u.m.n; i++)
		if (!strcmp(m->u.m.k[i], key)) {
			v_free(m->u.m.v[i]);
			m->u.m.v[i] = child;
			return;
		}
	if (m->u.m.n == m->u.m.cap) {
		m->u.m.cap = m->u.m.cap ? m->u.m.cap * 2 : 4;
		m->u.m.k = xrealloc(m->u.m.k, m->u.m.cap * sizeof *m->u.m.k);
		m->u.m.v = xrealloc(m->u.m.v, m->u.m.cap * sizeof *m->u.m.v);
	}
	size_t n = strlen(key);
	m->u.m.k[m->u.m.n] = xmalloc(n + 1);
	memcpy(m->u.m.k[m->u.m.n], key, n + 1);
	m->u.m.v[m->u.m.n++] = child;
}

bool v_map_del(struct value *m, const char *key)
{
	for (size_t i = 0; i < m->u.m.n; i++)
		if (!strcmp(m->u.m.k[i], key)) {
			free(m->u.m.k[i]);
			v_free(m->u.m.v[i]);
			memmove(&m->u.m.k[i], &m->u.m.k[i + 1], (m->u.m.n - i - 1) * sizeof *m->u.m.k);
			memmove(&m->u.m.v[i], &m->u.m.v[i + 1], (m->u.m.n - i - 1) * sizeof *m->u.m.v);
			m->u.m.n--;
			return true;
		}
	return false;
}

/* ---- buffer ---- */

void buf_add(struct buf *b, const void *p, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t c = b->cap ? b->cap : 64;
		while (c < b->len + n + 1)
			c *= 2;
		b->p = xrealloc(b->p, c);
		b->cap = c;
	}
	memcpy(b->p + b->len, p, n);
	b->len += n;
	b->p[b->len] = 0;
}

void buf_adds(struct buf *b, const char *s) { buf_add(b, s, strlen(s)); }

void buf_addf(struct buf *b, const char *fmt, ...)
{
	char tmp[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n < sizeof tmp) {
		buf_add(b, tmp, (size_t)n);
		return;
	}
	char *big = xmalloc((size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(big, (size_t)n + 1, fmt, ap);
	va_end(ap);
	buf_add(b, big, (size_t)n);
	free(big);
}

void buf_free(struct buf *b)
{
	free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
}

/* ---- canonical JSON writer ---- */

static void json_str(struct buf *b, const char *s, size_t n)
{
	static const char hex[] = "0123456789abcdef";
	buf_add(b, "\"", 1);
	size_t run = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		const char *esc = NULL;
		char u[7];
		if (c == '"') esc = "\\\"";
		else if (c == '\\') esc = "\\\\";
		else if (c == '\n') esc = "\\n";
		else if (c == '\t') esc = "\\t";
		else if (c == '\r') esc = "\\r";
		else if (c == '\b') esc = "\\b";
		else if (c == '\f') esc = "\\f";
		else if (c < 0x20) {
			u[0] = '\\'; u[1] = 'u'; u[2] = '0'; u[3] = '0';
			u[4] = hex[c >> 4]; u[5] = hex[c & 15]; u[6] = 0;
			esc = u;
		}
		if (esc) {
			buf_add(b, s + i - run, run);
			run = 0;
			buf_adds(b, esc);
		} else {
			run++;
		}
	}
	buf_add(b, s + n - run, run);
	buf_add(b, "\"", 1);
}

static int keycmp(const void *a, const void *b)
{
	return strcmp(**(char *const **)a, **(char *const **)b);
}

static void json_val(const struct value *v, struct buf *b)
{
	switch (v->t) {
	case V_NULL: buf_adds(b, "null"); break;
	case V_BOOL: buf_adds(b, v->u.b ? "true" : "false"); break;
	case V_INT: buf_addf(b, "%lld", v->u.i); break;
	case V_FLOAT:
		if (!isfinite(v->u.f))
			buf_adds(b, "null");
		else if (v->u.f == 0)
			buf_adds(b, "0.0"); /* -0 and 0 write the same */
		else {
			char tmp[40];
			snprintf(tmp, sizeof tmp, "%.17g", v->u.f);
			buf_adds(b, tmp);
			/* keep floats distinguishable from integers */
			if (!strpbrk(tmp, ".eE"))
				buf_adds(b, ".0");
		}
		break;
	case V_STR: json_str(b, v->u.s.p, v->u.s.len); break;
	case V_ARR:
		buf_add(b, "[", 1);
		for (size_t i = 0; i < v->u.a.n; i++) {
			if (i)
				buf_add(b, ",", 1);
			json_val(v->u.a.v[i], b);
		}
		buf_add(b, "]", 1);
		break;
	case V_MAP: {
		size_t n = v->u.m.n;
		char **ord[n ? n : 1];
		for (size_t i = 0; i < n; i++)
			ord[i] = &v->u.m.k[i];
		qsort(ord, n, sizeof ord[0], keycmp);
		buf_add(b, "{", 1);
		for (size_t i = 0; i < n; i++) {
			size_t idx = (size_t)(ord[i] - v->u.m.k);
			if (i)
				buf_add(b, ",", 1);
			json_str(b, v->u.m.k[idx], strlen(v->u.m.k[idx]));
			buf_add(b, ":", 1);
			json_val(v->u.m.v[idx], b);
		}
		buf_add(b, "}", 1);
		break;
	}
	}
}

bool v_to_json(const struct value *v, struct buf *out)
{
	json_val(v, out);
	if (!out->p)
		buf_add(out, "", 0);
	return true;
}

/* ---- UTF-8 ---- */

static size_t utf8_seq(const unsigned char *s, size_t len)
{
	unsigned c = s[0];
	if (c == 0)
		return 0;
	if (c < 0x80)
		return 1;
	size_t n;
	unsigned min, cp;
	if ((c & 0xe0) == 0xc0) { n = 2; min = 0x80; cp = c & 0x1f; }
	else if ((c & 0xf0) == 0xe0) { n = 3; min = 0x800; cp = c & 0x0f; }
	else if ((c & 0xf8) == 0xf0) { n = 4; min = 0x10000; cp = c & 0x07; }
	else return 0;
	if (n > len)
		return 0;
	for (size_t i = 1; i < n; i++) {
		if ((s[i] & 0xc0) != 0x80)
			return 0;
		cp = (cp << 6) | (s[i] & 0x3f);
	}
	if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
		return 0;
	return n;
}

bool utf8_valid(const char *s, size_t len)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t i = 0;
	while (i < len) {
		size_t n = utf8_seq(p + i, len - i);
		if (!n)
			return false;
		i += n;
	}
	return true;
}

size_t utf8_cut(const char *s, size_t len, size_t max)
{
	if (len <= max)
		return len;
	size_t i = max;
	while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80)
		i--;
	return i;
}

uint64_t hash64(const void *p, size_t n)
{
	const unsigned char *s = p;
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < n; i++) {
		h ^= s[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

/* ---- strict JSON reader ---- */

struct jp {
	const char *s, *e;
	int depth, max_depth;
	char *err;
	size_t errlen;
	bool failed;
};

static void jfail_at(struct jp *p, const char *at, const char *msg)
{
	if (!p->failed)
		snprintf(p->err, p->errlen, "%s at byte %zu", msg, (size_t)(at - p->s));
	p->failed = true;
}
#define jfail(p, msg) jfail_at((p), *c, (msg))

static void jws(struct jp *p, const char **c)
{
	while (*c < p->e && (**c == ' ' || **c == '\t' || **c == '\n' || **c == '\r'))
		(*c)++;
}

static struct value *jval(struct jp *p, const char **c);

static int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool jhex4(struct jp *p, const char **c, unsigned *out)
{
	if (p->e - *c < 4)
		return false;
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		int h = hexv((*c)[i]);
		if (h < 0)
			return false;
		v = v * 16 + (unsigned)h;
	}
	*c += 4;
	*out = v;
	return true;
}

static void put_utf8(struct buf *b, unsigned cp)
{
	char t[4];
	size_t n;
	if (cp < 0x80) { t[0] = (char)cp; n = 1; }
	else if (cp < 0x800) { t[0] = (char)(0xc0 | cp >> 6); t[1] = (char)(0x80 | (cp & 0x3f)); n = 2; }
	else if (cp < 0x10000) { t[0] = (char)(0xe0 | cp >> 12); t[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); t[2] = (char)(0x80 | (cp & 0x3f)); n = 3; }
	else { t[0] = (char)(0xf0 | cp >> 18); t[1] = (char)(0x80 | ((cp >> 12) & 0x3f)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); t[3] = (char)(0x80 | (cp & 0x3f)); n = 4; }
	buf_add(b, t, n);
}

static bool jstring(struct jp *p, const char **c, struct buf *out)
{
	(*c)++; /* opening quote */
	for (;;) {
		if (*c >= p->e) { jfail(p, "unterminated string"); return false; }
		unsigned char ch = (unsigned char)**c;
		if (ch == '"') { (*c)++; break; }
		if (ch < 0x20) { jfail(p, "control character in string"); return false; }
		if (ch == '\\') {
			(*c)++;
			if (*c >= p->e) { jfail(p, "bad escape"); return false; }
			char e = **c;
			(*c)++;
			switch (e) {
			case '"': buf_add(out, "\"", 1); break;
			case '\\': buf_add(out, "\\", 1); break;
			case '/': buf_add(out, "/", 1); break;
			case 'b': buf_add(out, "\b", 1); break;
			case 'f': buf_add(out, "\f", 1); break;
			case 'n': buf_add(out, "\n", 1); break;
			case 'r': buf_add(out, "\r", 1); break;
			case 't': buf_add(out, "\t", 1); break;
			case 'u': {
				unsigned cp, lo;
				if (!jhex4(p, c, &cp)) { jfail(p, "bad \\u escape"); return false; }
				if (cp >= 0xd800 && cp <= 0xdbff) {
					if (p->e - *c < 6 || (*c)[0] != '\\' || (*c)[1] != 'u') { jfail(p, "lone surrogate"); return false; }
					*c += 2;
					if (!jhex4(p, c, &lo) || lo < 0xdc00 || lo > 0xdfff) { jfail(p, "lone surrogate"); return false; }
					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
				} else if (cp >= 0xdc00 && cp <= 0xdfff) {
					jfail(p, "lone surrogate");
					return false;
				}
				if (cp == 0) { jfail(p, "NUL in string"); return false; }
				put_utf8(out, cp);
				break;
			}
			default: jfail(p, "bad escape"); return false;
			}
			continue;
		}
		size_t n = utf8_seq((const unsigned char *)*c, (size_t)(p->e - *c));
		if (!n) { jfail(p, "invalid UTF-8"); return false; }
		buf_add(out, *c, n);
		*c += n;
	}
	if (!out->p)
		buf_add(out, "", 0);
	return true;
}

static struct value *jnumber(struct jp *p, const char **c)
{
	const char *st = *c;
	bool isf = false;
	if (*c < p->e && **c == '-') (*c)++;
	if (*c >= p->e) { jfail(p, "bad number"); return NULL; }
	if (**c == '0') (*c)++;
	else if (**c >= '1' && **c <= '9') while (*c < p->e && **c >= '0' && **c <= '9') (*c)++;
	else { jfail(p, "bad number"); return NULL; }
	if (*c < p->e && **c == '.') {
		isf = true;
		(*c)++;
		if (*c >= p->e || **c < '0' || **c > '9') { jfail(p, "bad number"); return NULL; }
		while (*c < p->e && **c >= '0' && **c <= '9') (*c)++;
	}
	if (*c < p->e && (**c == 'e' || **c == 'E')) {
		isf = true;
		(*c)++;
		if (*c < p->e && (**c == '+' || **c == '-')) (*c)++;
		if (*c >= p->e || **c < '0' || **c > '9') { jfail(p, "bad number"); return NULL; }
		while (*c < p->e && **c >= '0' && **c <= '9') (*c)++;
	}
	char tmp[64];
	size_t n = (size_t)(*c - st);
	if (n >= sizeof tmp) { jfail(p, "number too long"); return NULL; }
	memcpy(tmp, st, n);
	tmp[n] = 0;
	if (!isf) {
		char *end;
		errno = 0;
		long long i = strtoll(tmp, &end, 10);
		if (*end || errno) { jfail(p, "integer out of range"); return NULL; }
		return v_int(i);
	}
	double d = strtod(tmp, NULL);
	if (!isfinite(d)) { jfail(p, "number out of range"); return NULL; }
	return v_float(d);
}

static bool jlit(struct jp *p, const char **c, const char *lit)
{
	size_t n = strlen(lit);
	if ((size_t)(p->e - *c) < n || memcmp(*c, lit, n))
		return false;
	*c += n;
	return true;
}

static struct value *jval(struct jp *p, const char **c)
{
	jws(p, c);
	if (*c >= p->e) { jfail(p, "unexpected end"); return NULL; }
	char ch = **c;
	if (ch == '{' || ch == '[') {
		if (++p->depth > p->max_depth) { jfail(p, "nested too deep"); return NULL; }
		(*c)++;
		bool obj = ch == '{';
		struct value *r = obj ? v_map() : v_arr();
		jws(p, c);
		if (*c < p->e && **c == (obj ? '}' : ']')) {
			(*c)++;
			p->depth--;
			return r;
		}
		for (;;) {
			jws(p, c);
			if (obj) {
				if (*c >= p->e || **c != '"') { jfail(p, "expected key"); v_free(r); return NULL; }
				struct buf k = {0};
				if (!jstring(p, c, &k)) { buf_free(&k); v_free(r); return NULL; }
				if (strlen(k.p) != k.len) { jfail(p, "NUL in key"); buf_free(&k); v_free(r); return NULL; }
				if (v_map_get(r, k.p)) { jfail(p, "duplicate key"); buf_free(&k); v_free(r); return NULL; }
				jws(p, c);
				if (*c >= p->e || **c != ':') { jfail(p, "expected ':'"); buf_free(&k); v_free(r); return NULL; }
				(*c)++;
				struct value *x = jval(p, c);
				if (!x) { buf_free(&k); v_free(r); return NULL; }
				v_map_set(r, k.p, x);
				buf_free(&k);
			} else {
				struct value *x = jval(p, c);
				if (!x) { v_free(r); return NULL; }
				v_arr_push(r, x);
			}
			jws(p, c);
			if (*c < p->e && **c == ',') { (*c)++; continue; }
			if (*c < p->e && **c == (obj ? '}' : ']')) { (*c)++; break; }
			jfail(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
			v_free(r);
			return NULL;
		}
		p->depth--;
		return r;
	}
	if (ch == '"') {
		struct buf b = {0};
		if (!jstring(p, c, &b)) { buf_free(&b); return NULL; }
		struct value *r = v_strn(b.p, b.len);
		buf_free(&b);
		return r;
	}
	if (ch == '-' || (ch >= '0' && ch <= '9'))
		return jnumber(p, c);
	if (jlit(p, c, "true")) return v_bool(true);
	if (jlit(p, c, "false")) return v_bool(false);
	if (jlit(p, c, "null")) return v_null();
	jfail(p, "unexpected character");
	return NULL;
}

struct value *v_from_json(const char *s, size_t len, int max_depth, char *err, size_t errlen)
{
	char dummy[1];
	struct jp p = { .s = s, .e = s + len, .max_depth = max_depth,
			.err = err ? err : dummy, .errlen = err ? errlen : sizeof dummy };
	const char *c = s;
	struct value *v = jval(&p, &c);
	if (v) {
		jws(&p, &c);
		if (c != p.e) {
			jfail_at(&p, c, "trailing data");
			v_free(v);
			v = NULL;
		}
	}
	if (v && err)
		err[0] = 0;
	return v;
}
