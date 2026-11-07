// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/toml.h"
#include "../src/value.h"
#include "t.h"

#include <stdlib.h>

static char *canon(const struct value *v)
{
	static struct buf b;
	buf_free(&b);
	v_to_json(v, &b);
	return b.p;
}

static void json_cases(void)
{
	char err[128];
	const char *src = "{\"b\":[1,2.5,\"x\\u00e9\"],\"a\":true, \"c\":null}";
	struct value *v = v_from_json(src, strlen(src), 8, err, sizeof err);
	CHECK(v);
	if (!v) {
		fprintf(stderr, "  %s\n", err);
		return;
	}
	if (v)
		CHECK_STR(canon(v), "{\"a\":true,\"b\":[1,2.5,\"x\xc3\xa9\"],\"c\":null}");
	/* the canonical form reads back to the same bytes */
	struct value *w = v_from_json(canon(v), strlen(canon(v)), 8, err, sizeof err);
	CHECK(w && v_equal(v, w));
	v_free(v);
	v_free(w);

	const char *bad[] = { "{\"a\":1,\"a\":2}", "[1,]", "{\"a\" 1}", "\"\\ud800\"", "01", "[1] x",
			      "\"a\x01\"", "\"\xff\"", "1e999", "[[[[[[[[[[1]]]]]]]]]]", "\"\\u0000\"", "99999999999999999999" };
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		struct value *x = v_from_json(bad[i], strlen(bad[i]), 8, err, sizeof err);
		CHECK(!x);
		if (x) {
			fprintf(stderr, "  accepted: %s\n", bad[i]);
			v_free(x);
		}
	}

	/* floats stay floats, integers stay integers, map keys sort */
	struct value *m = v_map();
	v_map_set(m, "z", v_float(1));
	v_map_set(m, "a", v_int(1));
	v_map_set(m, "m", v_str("q\"\n\x02"));
	CHECK_STR(canon(m), "{\"a\":1,\"m\":\"q\\\"\\n\\u0002\",\"z\":1.0}");
	v_free(m);
}

static void utf8_cases(void)
{
	CHECK(utf8_valid("abc\xc3\xa9", 5));
	CHECK(!utf8_valid("\xc0\x80", 2));          /* overlong NUL */
	CHECK(!utf8_valid("\xed\xa0\x80", 3));      /* surrogate */
	CHECK(!utf8_valid("a\0b", 3));
	CHECK(!utf8_valid("\xe2\x82", 2));          /* cut */
	CHECK_INT(utf8_cut("a\xc3\xa9", 3, 2), 1);  /* never splits a code point */
	CHECK_INT(utf8_cut("abc", 3, 5), 3);
}

static void toml_cases(void)
{
	char err[128];
	const char *doc =
		"# a widget\n"
		"name = \"Continue reading\"   # trailing comment\n"
		"sizes = [ \"wide\",\n  \"half\", ]\n"
		"refresh = { on_change = true, every = '1h' }\n"
		"n = -12\n"
		"f = 0.25\n"
		"\"quoted key\" = \"\\u00e9\\t\"\n"
		"[props]\n"
		"category = \"Games\"\n"
		"[[widget]]\n"
		"id = \"a\"\n"
		"[[widget]]\n"
		"id = \"b\"\n";
	struct value *v = toml_parse(doc, strlen(doc), err, sizeof err);
	CHECK(v);
	if (!v) {
		fprintf(stderr, "  %s\n", err);
		return;
	}
	CHECK_STR(canon(v), "{\"f\":0.25,\"n\":-12,\"name\":\"Continue reading\",\"props\":{\"category\":\"Games\"},"
			    "\"quoted key\":\"\xc3\xa9\\t\",\"refresh\":{\"every\":\"1h\",\"on_change\":true},"
			    "\"sizes\":[\"wide\",\"half\"],\"widget\":[{\"id\":\"a\"},{\"id\":\"b\"}]}");
	v_free(v);

	struct { const char *doc, *msg; } bad[] = {
		{ "a = 1\na = 2\n", "line 2: duplicate key" },
		{ "a = 1 b = 2\n", "line 1: expected end of line" },
		{ "a.b = 1\n", "line 1: dotted keys are not supported" },
		{ "a = 0x10\n", "line 1: bad number (only decimal numbers are supported)" },
		{ "a = inf\n", "line 1: bad value" },
		{ "a = \"\"\"x\"\"\"\n", "line 1: multi-line strings are not supported" },
		{ "a = \"x\n", "line 1: unterminated string" },
		{ "[t]\n[t]\n", "line 2: table defined twice" },
		{ "a = 1979-05-27\n", "line 1: bad number (only decimal numbers are supported)" },
		{ "a = { b = 1,\n c = 2 }\n", "line 1: expected a key" },
		{ "a = [1, 2\n", "line 2: expected ',' or ']'" },
		{ "\n\n= 3\n", "line 3: expected a key" },
		{ "a = \"\\q\"\n", "line 1: bad escape" },
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		struct value *x = toml_parse(bad[i].doc, strlen(bad[i].doc), err, sizeof err);
		CHECK(!x);
		v_free(x);
		CHECK_STR(err, bad[i].msg);
	}

	/* the writer's output reads back */
	struct value *m = v_map();
	v_map_set(m, "s", v_str("a\"b\\c\n"));
	v_map_set(m, "x y", v_float(3));
	v_map_set(m, "l", v_bool(false));
	struct buf b = {0};
	CHECK(toml_put_value(&b, m));
	struct value *r = toml_parse_value(b.p, b.len, err, sizeof err);
	CHECK(r && v_equal(r, m));
	buf_free(&b);
	v_free(r);
	v_free(m);
}

int main(void)
{
	json_cases();
	utf8_cases();
	toml_cases();
	return t_done("value");
}
