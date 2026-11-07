// SPDX-License-Identifier: GPL-2.0-or-later
#include "toml.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 16

struct tp {
	const char *s, *c, *e;
	int line;
	char *err;
	size_t errlen;
	bool failed;
};

static void tfail(struct tp *p, const char *msg)
{
	if (!p->failed)
		snprintf(p->err, p->errlen, "line %d: %s", p->line, msg);
	p->failed = true;
}

static bool at_end(struct tp *p) { return p->c >= p->e; }

/* spaces and tabs */
static void sp(struct tp *p)
{
	while (!at_end(p) && (*p->c == ' ' || *p->c == '\t'))
		p->c++;
}

static void comment(struct tp *p)
{
	if (!at_end(p) && *p->c == '#') {
		while (!at_end(p) && *p->c != '\n') {
			unsigned char ch = (unsigned char)*p->c;
			if (ch < 0x20 && ch != '\t') { tfail(p, "control character in comment"); return; }
			p->c++;
		}
	}
}

/* end of line: optional spaces, comment, then newline or end of input */
static bool eol(struct tp *p)
{
	sp(p);
	comment(p);
	if (p->failed)
		return false;
	if (at_end(p))
		return true;
	if (*p->c == '\r' && p->c + 1 < p->e && p->c[1] == '\n')
		p->c++;
	if (*p->c == '\n') {
		p->c++;
		p->line++;
		return true;
	}
	tfail(p, "expected end of line");
	return false;
}

/* whitespace, newlines and comments inside arrays */
static void ws_nl(struct tp *p)
{
	for (;;) {
		sp(p);
		comment(p);
		if (p->failed || at_end(p))
			return;
		if (*p->c == '\r' && p->c + 1 < p->e && p->c[1] == '\n')
			p->c++;
		if (*p->c != '\n')
			return;
		p->c++;
		p->line++;
	}
}

static bool bare_char(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static void put_utf8(struct buf *b, unsigned long cp)
{
	char t[4];
	size_t n;
	if (cp < 0x80) { t[0] = (char)cp; n = 1; }
	else if (cp < 0x800) { t[0] = (char)(0xc0 | cp >> 6); t[1] = (char)(0x80 | (cp & 0x3f)); n = 2; }
	else if (cp < 0x10000) { t[0] = (char)(0xe0 | cp >> 12); t[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); t[2] = (char)(0x80 | (cp & 0x3f)); n = 3; }
	else { t[0] = (char)(0xf0 | cp >> 18); t[1] = (char)(0x80 | ((cp >> 12) & 0x3f)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); t[3] = (char)(0x80 | (cp & 0x3f)); n = 4; }
	buf_add(b, t, n);
}

static bool string_lit(struct tp *p, struct buf *out)
{
	char q = *p->c;
	if (p->e - p->c >= 3 && p->c[1] == q && p->c[2] == q) {
		tfail(p, "multi-line strings are not supported");
		return false;
	}
	p->c++;
	const char *start = p->c;
	for (;;) {
		if (at_end(p) || *p->c == '\n') { tfail(p, "unterminated string"); return false; }
		unsigned char ch = (unsigned char)*p->c;
		if (ch == (unsigned char)q) { p->c++; break; }
		if ((ch < 0x20 && ch != '\t') || ch == 0x7f) { tfail(p, "control character in string"); return false; }
		if (q == '"' && ch == '\\') {
			p->c++;
			if (at_end(p)) { tfail(p, "bad escape"); return false; }
			char e = *p->c++;
			int nhex = 0;
			switch (e) {
			case 'b': buf_add(out, "\b", 1); break;
			case 't': buf_add(out, "\t", 1); break;
			case 'n': buf_add(out, "\n", 1); break;
			case 'f': buf_add(out, "\f", 1); break;
			case 'r': buf_add(out, "\r", 1); break;
			case '"': buf_add(out, "\"", 1); break;
			case '\\': buf_add(out, "\\", 1); break;
			case 'u': nhex = 4; break;
			case 'U': nhex = 8; break;
			default: tfail(p, "bad escape"); return false;
			}
			if (nhex) {
				unsigned long cp = 0;
				for (int i = 0; i < nhex; i++) {
					if (at_end(p)) { tfail(p, "bad unicode escape"); return false; }
					char h = *p->c++;
					int v = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
					if (v < 0) { tfail(p, "bad unicode escape"); return false; }
					cp = cp * 16 + (unsigned long)v;
				}
				if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) { tfail(p, "bad unicode escape"); return false; }
				put_utf8(out, cp);
			}
			continue;
		}
		buf_add(out, p->c, 1);
		p->c++;
	}
	(void)start;
	if (!out->p)
		buf_add(out, "", 0);
	if (!utf8_valid(out->p, out->len)) { tfail(p, "invalid UTF-8 in string"); return false; }
	return true;
}

static bool key(struct tp *p, struct buf *out)
{
	if (at_end(p)) { tfail(p, "expected a key"); return false; }
	if (*p->c == '"' || *p->c == '\'') {
		if (!string_lit(p, out))
			return false;
		if (strlen(out->p) != out->len || !out->len) { tfail(p, "bad key"); return false; }
	} else {
		const char *st = p->c;
		while (!at_end(p) && bare_char(*p->c))
			p->c++;
		if (p->c == st) { tfail(p, "expected a key"); return false; }
		buf_add(out, st, (size_t)(p->c - st));
	}
	sp(p);
	if (!at_end(p) && *p->c == '.') { tfail(p, "dotted keys are not supported"); return false; }
	return true;
}

static struct value *value(struct tp *p, int depth);

static struct value *number(struct tp *p)
{
	const char *st = p->c;
	while (!at_end(p) && (bare_char(*p->c) || *p->c == '.' || *p->c == '+'))
		p->c++;
	size_t n = (size_t)(p->c - st);
	char tmp[64];
	if (!n || n >= sizeof tmp) { tfail(p, "bad value"); return NULL; }
	memcpy(tmp, st, n);
	tmp[n] = 0;
	/* strict decimal: [+-]? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE][+-]?[0-9]+)? */
	const char *q = tmp;
	if (*q == '+' || *q == '-') q++;
	if (*q == '0') q++;
	else if (*q >= '1' && *q <= '9') while (*q >= '0' && *q <= '9') q++;
	else { tfail(p, "bad value"); return NULL; }
	bool isf = false;
	if (*q == '.') {
		isf = true;
		q++;
		if (*q < '0' || *q > '9') { tfail(p, "bad number"); return NULL; }
		while (*q >= '0' && *q <= '9') q++;
	}
	if (*q == 'e' || *q == 'E') {
		isf = true;
		q++;
		if (*q == '+' || *q == '-') q++;
		if (*q < '0' || *q > '9') { tfail(p, "bad number"); return NULL; }
		while (*q >= '0' && *q <= '9') q++;
	}
	if (*q) { tfail(p, "bad number (only decimal numbers are supported)"); return NULL; }
	errno = 0;
	if (!isf) {
		long long i = strtoll(tmp, NULL, 10);
		if (errno) { tfail(p, "integer out of range"); return NULL; }
		return v_int(i);
	}
	double d = strtod(tmp, NULL);
	if (!isfinite(d)) { tfail(p, "number out of range"); return NULL; }
	return v_float(d);
}

static struct value *inline_table(struct tp *p, int depth)
{
	p->c++; /* { */
	struct value *m = v_map();
	sp(p);
	if (!at_end(p) && *p->c == '}') { p->c++; return m; }
	for (;;) {
		sp(p);
		struct buf k = {0};
		if (!key(p, &k)) { buf_free(&k); v_free(m); return NULL; }
		if (v_map_get(m, k.p)) { tfail(p, "duplicate key"); buf_free(&k); v_free(m); return NULL; }
		sp(p);
		if (at_end(p) || *p->c != '=') { tfail(p, "expected '='"); buf_free(&k); v_free(m); return NULL; }
		p->c++;
		sp(p);
		struct value *v = value(p, depth + 1);
		if (!v) { buf_free(&k); v_free(m); return NULL; }
		v_map_set(m, k.p, v);
		buf_free(&k);
		sp(p);
		if (!at_end(p) && *p->c == ',') { p->c++; continue; }
		if (!at_end(p) && *p->c == '}') { p->c++; return m; }
		tfail(p, "expected ',' or '}' (inline tables stay on one line)");
		v_free(m);
		return NULL;
	}
}

static struct value *array(struct tp *p, int depth)
{
	p->c++; /* [ */
	struct value *a = v_arr();
	for (;;) {
		ws_nl(p);
		if (p->failed) { v_free(a); return NULL; }
		if (!at_end(p) && *p->c == ']') { p->c++; return a; }
		struct value *v = value(p, depth + 1);
		if (!v) { v_free(a); return NULL; }
		v_arr_push(a, v);
		ws_nl(p);
		if (p->failed) { v_free(a); return NULL; }
		if (!at_end(p) && *p->c == ',') { p->c++; continue; }
		if (!at_end(p) && *p->c == ']') { p->c++; return a; }
		tfail(p, "expected ',' or ']'");
		v_free(a);
		return NULL;
	}
}

static struct value *value(struct tp *p, int depth)
{
	if (depth > MAX_DEPTH) { tfail(p, "nested too deep"); return NULL; }
	if (at_end(p)) { tfail(p, "expected a value"); return NULL; }
	char ch = *p->c;
	if (ch == '"' || ch == '\'') {
		struct buf b = {0};
		if (!string_lit(p, &b)) { buf_free(&b); return NULL; }
		struct value *v = v_strn(b.p, b.len);
		buf_free(&b);
		return v;
	}
	if (ch == '[') return array(p, depth);
	if (ch == '{') return inline_table(p, depth);
	if (p->e - p->c >= 4 && !memcmp(p->c, "true", 4) && (p->e - p->c == 4 || !bare_char(p->c[4]))) {
		p->c += 4;
		return v_bool(true);
	}
	if (p->e - p->c >= 5 && !memcmp(p->c, "false", 5) && (p->e - p->c == 5 || !bare_char(p->c[5]))) {
		p->c += 5;
		return v_bool(false);
	}
	if (ch == '+' || ch == '-' || (ch >= '0' && ch <= '9'))
		return number(p);
	tfail(p, "bad value");
	return NULL;
}

struct value *toml_parse(const char *s, size_t len, char *err, size_t errlen)
{
	char dummy[1];
	struct tp p = { .s = s, .c = s, .e = s + len, .line = 1,
			.err = err ? err : dummy, .errlen = err ? errlen : sizeof dummy };
	if (memchr(s, 0, len)) {
		tfail(&p, "NUL byte in file");
		return NULL;
	}
	if (!utf8_valid(s, len)) {
		tfail(&p, "file is not valid UTF-8");
		return NULL;
	}
	struct value *root = v_map();
	struct value *cur = root;
	struct value *defined = v_map(); /* names of [table] headers seen */
	while (!p.failed) {
		ws_nl(&p);
		if (p.failed || at_end(&p))
			break;
		if (*p.c == '[') {
			bool aot = p.e - p.c >= 2 && p.c[1] == '[';
			p.c += aot ? 2 : 1;
			sp(&p);
			struct buf k = {0};
			if (!key(&p, &k)) { buf_free(&k); break; }
			sp(&p);
			if (aot ? (p.e - p.c < 2 || p.c[0] != ']' || p.c[1] != ']') : (at_end(&p) || *p.c != ']')) {
				tfail(&p, aot ? "expected ']]'" : "expected ']'");
				buf_free(&k);
				break;
			}
			p.c += aot ? 2 : 1;
			struct value *ex = v_map_get(root, k.p);
			if (aot) {
				if (ex && (ex->t != V_ARR || !v_map_get(defined, k.p))) { tfail(&p, "key already defined"); buf_free(&k); break; }
				if (!ex) {
					ex = v_arr();
					v_map_set(root, k.p, ex);
					v_map_set(defined, k.p, v_bool(true));
				}
				cur = v_map();
				v_arr_push(ex, cur);
			} else {
				if (ex) { tfail(&p, "table defined twice"); buf_free(&k); break; }
				cur = v_map();
				v_map_set(root, k.p, cur);
			}
			buf_free(&k);
			eol(&p);
			continue;
		}
		struct buf k = {0};
		if (!key(&p, &k)) { buf_free(&k); break; }
		if (v_map_get(cur, k.p)) { tfail(&p, "duplicate key"); buf_free(&k); break; }
		sp(&p);
		if (at_end(&p) || *p.c != '=') { tfail(&p, "expected '='"); buf_free(&k); break; }
		p.c++;
		sp(&p);
		struct value *v = value(&p, 0);
		if (!v) { buf_free(&k); break; }
		v_map_set(cur, k.p, v);
		buf_free(&k);
		eol(&p);
	}
	v_free(defined);
	if (p.failed) {
		v_free(root);
		return NULL;
	}
	if (err)
		err[0] = 0;
	return root;
}

struct value *toml_parse_value(const char *s, size_t len, char *err, size_t errlen)
{
	char dummy[1];
	struct tp p = { .s = s, .c = s, .e = s + len, .line = 1,
			.err = err ? err : dummy, .errlen = err ? errlen : sizeof dummy };
	if (memchr(s, 0, len) || !utf8_valid(s, len)) {
		tfail(&p, "not valid UTF-8 text");
		return NULL;
	}
	sp(&p);
	struct value *v = value(&p, 0);
	if (v) {
		sp(&p);
		if (!at_end(&p)) {
			tfail(&p, "trailing data");
			v_free(v);
			return NULL;
		}
	}
	return v;
}

/* ---- writer ---- */

static void put_str(struct buf *b, const char *s, size_t n)
{
	buf_add(b, "\"", 1);
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"') buf_adds(b, "\\\"");
		else if (c == '\\') buf_adds(b, "\\\\");
		else if (c == '\n') buf_adds(b, "\\n");
		else if (c == '\t') buf_adds(b, "\\t");
		else if (c < 0x20 || c == 0x7f) buf_addf(b, "\\u%04x", c);
		else buf_add(b, &s[i], 1);
	}
	buf_add(b, "\"", 1);
}

void toml_put_key(struct buf *b, const char *key)
{
	bool bare = *key != 0;
	for (const char *q = key; *q; q++)
		if (!bare_char(*q))
			bare = false;
	if (bare)
		buf_adds(b, key);
	else
		put_str(b, key, strlen(key));
}

bool toml_put_value(struct buf *b, const struct value *v)
{
	switch (v->t) {
	case V_NULL:
		return false;
	case V_BOOL:
		buf_adds(b, v->u.b ? "true" : "false");
		return true;
	case V_INT:
		buf_addf(b, "%lld", v->u.i);
		return true;
	case V_FLOAT: {
		if (!isfinite(v->u.f))
			return false;
		char tmp[40];
		snprintf(tmp, sizeof tmp, "%.17g", v->u.f);
		buf_adds(b, tmp);
		if (!strpbrk(tmp, ".eE"))
			buf_adds(b, ".0");
		return true;
	}
	case V_STR:
		put_str(b, v->u.s.p, v->u.s.len);
		return true;
	case V_ARR:
		buf_add(b, "[", 1);
		for (size_t i = 0; i < v->u.a.n; i++) {
			if (i)
				buf_adds(b, ", ");
			if (!toml_put_value(b, v->u.a.v[i]))
				return false;
		}
		buf_add(b, "]", 1);
		return true;
	case V_MAP:
		buf_add(b, "{", 1);
		for (size_t i = 0; i < v->u.m.n; i++) {
			buf_adds(b, i ? ", " : " ");
			toml_put_key(b, v->u.m.k[i]);
			buf_adds(b, " = ");
			if (!toml_put_value(b, v->u.m.v[i]))
				return false;
		}
		buf_adds(b, v->u.m.n ? " }" : "}");
		return true;
	}
	return false;
}
