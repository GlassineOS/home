// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/manifest.h"
#include "../src/pack.h"
#include "t.h"

static const char *good =
	"name = \"App category\"\n"
	"icon = \"apps\"\n"
	"sizes = [\"half\", \"wide\"]\n"
	"reads = [\"~/.config/koreader/history.lua\", \"~/Books/\"]\n"
	"refresh = { on_change = true, every = \"1h\" }\n"
	"[props]\n"
	"category = \"Games\"\n"
	"columns = 2\n"
	"scale = 1.5\n";

static void parse_ok(void)
{
	struct manifest m;
	char err[256];
	bool ok = manifest_parse(good, strlen(good), &m, err, sizeof err);
	CHECK(ok);
	if (!ok) {
		fprintf(stderr, "  %s\n", err);
		return;
	}
	CHECK_STR(m.name, "App category");
	CHECK_INT(m.nsizes, 2);
	CHECK_INT(m.sizes[0], SIZE_HALF);
	CHECK_INT(m.nreads, 2);
	CHECK(!m.reads[0].dir && m.reads[1].dir);
	CHECK(m.on_change);
	CHECK_INT(m.every_s, 3600);
	CHECK_INT(m.format, 1);

	/* settings: defaults, saved values of the right type, ui.set */
	struct value *saved = v_map();
	v_map_set(saved, "category", v_str("Office"));
	v_map_set(saved, "columns", v_str("three")); /* wrong type: the default wins */
	v_map_set(saved, "scale", v_int(2));         /* an integer for a float setting */
	v_map_set(saved, "gone", v_int(1));          /* no longer declared: dropped */
	struct value *p = props_effective(&m, saved);
	struct buf b = {0};
	v_to_json(p, &b);
	CHECK_STR(b.p, "{\"category\":\"Office\",\"columns\":2,\"scale\":2.0}");
	buf_free(&b);

	struct value *set = v_map();
	v_map_set(set, "category", v_str("Games"));
	CHECK(props_apply(&m, p, set, err, sizeof err));
	CHECK_STR(v_map_get(p, "category")->u.s.p, "Games");
	v_map_set(set, "nope", v_int(1));
	CHECK(!props_apply(&m, p, set, err, sizeof err));
	CHECK_STR(err, "ui.set: \"nope\" is not a setting of this widget");
	v_free(set);
	set = v_map();
	v_map_set(set, "columns", v_bool(true));
	CHECK(!props_apply(&m, p, set, err, sizeof err));
	CHECK_STR(err, "ui.set: \"columns\" has the wrong type");
	v_free(set);
	v_free(p);
	v_free(saved);
	manifest_free(&m);

	/* the smallest manifest */
	const char *min = "name = \"Clock\"\nicon = \"schedule\"\nsizes = [\"wide\"]\n";
	CHECK(manifest_parse(min, strlen(min), &m, err, sizeof err));
	CHECK_INT(m.nreads, 0);
	CHECK_INT(m.every_s, 0);
	manifest_free(&m);
}

static void parse_errors(void)
{
	static const struct { const char *doc, *msg; } cases[] = {
		{ "icon = \"x\"\nsizes = [\"wide\"]\n", "name: missing" },
		{ "name = 3\nicon = \"x\"\nsizes = [\"wide\"]\n", "name: must be a string" },
		{ "name = \"a\"\nicon = \"X!\"\nsizes = [\"wide\"]\n", "icon: an icon name of [a-z0-9_-]" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"tall\"]\n", "sizes: one or two of \"half\", \"wide\"" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\", \"wide\"]\n", "sizes: one or two of \"half\", \"wide\"" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = []\n", "sizes: one or two of \"half\", \"wide\"" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\nrender = \"x\"\n", "render: unknown key" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\nnet = [\"example.org\"]\n", "net: unknown key" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\nformat = 2\n", "format: this widget needs a newer Home" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\nreads = [\"~/x\"]\nrefresh = { every = \"10m\" }\n",
		  "refresh.every: at least 15 minutes" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\nrefresh = { every = \"20m\" }\n",
		  "refresh.every: a multiple of 15 minutes" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\nrefresh = { every = \"soon\" }\n",
		  "refresh.every: a duration like \"15m\", \"1h\" or \"1d\"" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\nrefresh = { on_change = true }\n",
		  "refresh: on_change needs files in reads" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\nrefresh = { often = true }\n", "refresh.often: unknown key" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\n[props]\nList = 1\n",
		  "props.List: setting names are [a-z_][a-z0-9_]*, at most 32 bytes" },
		{ "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\n[props]\nl = [1]\n",
		  "props.l: a default must be a string, number or boolean" },
		{ "name = \"a\"\nname = \"b\"\n", "line 2: duplicate key" },
	};
	char err[256];
	struct manifest m;
	for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
		bool ok = manifest_parse(cases[i].doc, strlen(cases[i].doc), &m, err, sizeof err);
		CHECK(!ok);
		if (ok)
			manifest_free(&m);
		CHECK_STR(err, cases[i].msg);
	}

	/* reads that are refused */
	static const char *const bad_reads[] = { "relative/x", "~/../etc/passwd", "/etc/./passwd", "/a//b", "/",
						 "/proc/self/environ", "/proc", "/sys/", "/dev/mem", "/run/x", "~user/x", "" };
	for (size_t i = 0; i < sizeof bad_reads / sizeof *bad_reads; i++) {
		char doc[512];
		snprintf(doc, sizeof doc, "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\nreads = [\"%s\"]\n", bad_reads[i]);
		bool ok = manifest_parse(doc, strlen(doc), &m, err, sizeof err);
		CHECK(!ok);
		if (ok) {
			fprintf(stderr, "  accepted read: %s\n", bad_reads[i]);
			manifest_free(&m);
		}
	}
	/* the processes' neighbours are fine: /procfoo is not /proc */
	const char *doc = "name = \"a\"\nicon = \"x\"\nsizes = [\"wide\"]\nreads = [\"/procfoo/x\", \"~/\"]\n";
	CHECK(manifest_parse(doc, strlen(doc), &m, err, sizeof err));
	manifest_free(&m);
}

static void durations(void)
{
	CHECK_INT(parse_duration("15m"), 900);
	CHECK_INT(parse_duration("2h30m"), 9000);
	CHECK_INT(parse_duration("1d"), 86400);
	CHECK_INT(parse_duration("1"), -1);
	CHECK_INT(parse_duration("h"), -1);
	CHECK_INT(parse_duration(""), -1);
	CHECK_INT(parse_duration("999999999d"), -1);
	CHECK(widget_id_valid("reading"));
	CHECK(!widget_id_valid("../x"));
	CHECK(!widget_id_valid(".hidden"));
	CHECK(!widget_id_valid("Big"));
}

int main(void)
{
	parse_ok();
	parse_errors();
	durations();
	return t_done("manifest");
}
