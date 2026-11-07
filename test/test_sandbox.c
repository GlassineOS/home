// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sandbox tests: real runs through the zygote and the seccomp filter, against widgets
 * in test/widgets and a throwaway HOME under out/.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lauxlib.h>
#include <lua.h>

#include "../src/manifest.h"
#include "../src/sandbox.h"
#include "../src/sched.h"
#include "t.h"

static struct zygote z;
static char home[PATH_MAX];

static void put(const char *rel, const char *content)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/%s", home, rel);
	FILE *f = fopen(p, "w");
	if (!f) {
		perror(p);
		exit(2);
	}
	fputs(content, f);
	fclose(f);
}

static void sh(const char *cmd)
{
	if (system(cmd)) {
		fprintf(stderr, "failed: %s\n", cmd);
		exit(2);
	}
}

static int dump_writer(lua_State *L, const void *p, size_t n, void *ud)
{
	fwrite(p, 1, n, ud);
	return 0;
}

static void put_bytecode(const char *rel, const char *src)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/%s", home, rel);
	lua_State *L = luaL_newstate();
	luaL_loadstring(L, src);
	FILE *f = fopen(p, "wb");
	lua_dump(L, dump_writer, f, 0);
	fclose(f);
	lua_close(L);
}

static void setup_home(void)
{
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof cwd))
		exit(2);
	snprintf(home, sizeof home, "%s/out/test-home", cwd);
	char cmd[PATH_MAX * 2];
	snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s/data/sub' '%s/widgets'", home, home, home);
	sh(cmd);
	put("secret.txt", "secret");
	put("single.txt", "single");
	put("data/ok.txt", "hello");
	put("data/sub/inner.txt", "inner");
	put("data/plain.lua", "-- a data file\nreturn { title = \"A\", n = 3, list = { \"x\", \"y\" }, [7] = true }\n");
	put("data/code.lua", "os.execute(\"touch /tmp/pwned\")\nreturn 1\n");
	put("data/load.lua", "return load(\"return 1\")()\n");
	put("data/strmeta.lua", "return (\"x\"):rep(3)\n");
	put("data/funcs.lua", "return { x = 1, f = function() return 2 end, t = { g = function() end, y = 2 } }\n");
	put("data/loop.lua", "while true do end\n");
	put("data/inf.lua", "return { a = inf, b = -inf, c = nan, d = -nan, e = 0/0 }\n");
	put("data/huge.lua", "local s = \"x\" for i = 1, 30 do s = s .. s end return s\n");
	put("data/cyclic.lua", "local t = { name = \"c\" } t.self = t return t\n");
	put("data/syntax.lua", "return {\n");
	put("data/many.lua", "local t = {} for i = 1, 40000 do t[i] = {} end return t\n");
	put("data/deep.lua", "local t = {} for i = 1, 100 do t = { t } end return t\n");
	put_bytecode("data/bytecode.lua", "return { pwned = true }");
	snprintf(cmd, sizeof cmd,
		 "cd '%s' && ln -s ../secret.txt data/link && ln -s .. data/linkdir && ln -s sub data/sublink && "
		 "mkfifo data/fifo && head -c 3000000 /dev/zero > data/big.txt && "
		 "mkdir -p widgets/binary && cp '%s/test/widgets/hello/widget.toml' widgets/binary/",
		 home, cwd);
	sh(cmd);
	put_bytecode("widgets/binary/widget.lua", "return { render = function() return { ui.title('x') } end }");
}

struct out {
	enum run_status status;
	char text[1024];
	uint64_t hash;
	size_t nfiles;
	char files[4][256];
	size_t nlog;
	char log0[400]; /* the first print line */
};

static struct out run(const char *dir, const char *props_json, int hook, bool fallback, bool edit, const char *apps)
{
	struct out o = { 0 };
	char err[512];
	struct manifest m;
	if (!manifest_load(dir, &m, err, sizeof err)) {
		fprintf(stderr, "%s: %s\n", dir, err);
		o.status = RUN_CRASH;
		snprintf(o.text, sizeof o.text, "manifest: %s", err);
		return o;
	}
	struct value *saved = props_json ? v_from_json(props_json, strlen(props_json), 8, NULL, 0) : NULL;
	struct value *props = props_effective(&m, saved);
	struct value *ap = apps ? v_from_json(apps, strlen(apps), 8, NULL, 0) : NULL;
	struct run_opts opts = { .now = 1700000000, .home = home, .test_hook = hook, .force_fallback = fallback,
				 .edit = edit, .apps = ap, .wall_ms = hook == HOOK_BLOCK ? 500 : 0 };
	struct run_result r;
	run_widget(&z, dir, &m, props, &opts, &r);
	o.status = r.status;
	snprintf(o.text, sizeof o.text, "%s", r.status == RUN_OK ? r.json.p : r.msg);
	o.hash = r.hash;
	o.nfiles = r.nfiles;
	for (size_t i = 0; i < r.nfiles && i < 4; i++)
		snprintf(o.files[i], sizeof o.files[i], "%s", r.files[i] + strlen(home));
	o.nlog = r.nlog;
	if (r.nlog)
		snprintf(o.log0, sizeof o.log0, "%s", r.log[0]);
	run_result_free(&r);
	v_free(props);
	v_free(saved);
	v_free(ap);
	manifest_free(&m);
	return o;
}

#define W(name) "test/widgets/" name

/* the text of a single ui.text output */
static const char *text_of(const struct out *o)
{
	static char t[1024];
	const char *p = strstr(o->text, "\"text\":\"");
	if (o->status != RUN_OK || !p)
		return o->text;
	snprintf(t, sizeof t, "%s", p + 8);
	char *e = strstr(t, "\"}");
	if (e)
		*e = 0;
	return t;
}

static void files(void)
{
	for (int fb = 0; fb < 2; fb++) {
		struct out o = run(W("files"), NULL, 0, fb, false, NULL);
		CHECK_STR(text_of(&o), "read: hello");
		CHECK_INT(o.nfiles, 1);
		CHECK_STR(o.files[0], "/data/ok.txt");

		o = run(W("files"), "{\"path\":\"~/data/sub/inner.txt\"}", 0, fb, false, NULL);
		CHECK_STR(text_of(&o), "read: inner");
		o = run(W("files"), "{\"path\":\"~/single.txt\"}", 0, fb, false, NULL);
		CHECK_STR(text_of(&o), "read: single");

		/* an unlisted file: the run fails, nothing is read */
		o = run(W("files"), "{\"path\":\"~/secret.txt\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		CHECK(strstr(o.text, "not in the widget's reads") != NULL);
		CHECK_INT(o.nfiles, 0);

		/* .. is refused before anything is opened */
		o = run(W("files"), "{\"path\":\"~/data/../secret.txt\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		CHECK(strstr(o.text, "not a valid path") != NULL);
		o = run(W("files"), "{\"path\":\"/etc/passwd\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		o = run(W("files"), "{\"path\":\"data/ok.txt\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);

		/* symlinks inside a read are never followed, whether file or directory */
		o = run(W("files"), "{\"path\":\"~/data/link\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		CHECK(strstr(o.text, "symlink") != NULL);
		o = run(W("files"), "{\"path\":\"~/data/linkdir/secret.txt\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		o = run(W("files"), "{\"path\":\"~/data/sublink/inner.txt\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);

		/* a FIFO is not opened for blocking reads; a large file is refused */
		o = run(W("files"), "{\"path\":\"~/data/fifo\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		CHECK(strstr(o.text, "not a regular file") != NULL);
		o = run(W("files"), "{\"path\":\"~/data/big.txt\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		CHECK(strstr(o.text, "larger than 2 MB") != NULL);

		/* a missing file is not an error: nil + reason, and it is still recorded for watching */
		o = run(W("files"), "{\"path\":\"~/data/later.txt\"}", 0, fb, false, NULL);
		CHECK_STR(text_of(&o), "nil: no such file");
		CHECK_INT(o.nfiles, 1);
	}
}

/* without /proc the real path of what was opened can't be checked: the read is refused */
static void no_proc(void)
{
	char err[512];
	struct manifest m;
	if (!manifest_load(W("files"), &m, err, sizeof err)) {
		fprintf(stderr, "files: %s\n", err);
		CHECK(false);
		return;
	}
	for (int np = 0; np < 2; np++) {
		struct broker b;
		broker_init(&b, &m, home);
		b.test_no_proc = np;
		long long size = 0;
		int fd = broker_open(&b, "~/data/ok.txt", &size);
		if (np) {
			CHECK_INT(fd, -1);
			CHECK_INT(errno, EACCES);
		} else {
			CHECK(fd >= 0 && size == 5);
		}
		if (fd >= 0)
			close(fd);
		broker_free(&b);
	}
	manifest_free(&m);
}

/* HOME=/: "~/" is then "/", which covers nothing, as "/" itself can't be listed */
static void home_is_root(void)
{
	char saved[PATH_MAX];
	snprintf(saved, sizeof saved, "%s", home);
	snprintf(home, sizeof home, "/");
	for (int fb = 0; fb < 2; fb++) {
		struct out o = run(W("homeread"), "{\"path\":\"~/etc/hostname\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		CHECK(strstr(o.text, "not in the widget's reads") != NULL);
		o = run(W("homeread"), "{\"path\":\"~/proc/version\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		CHECK(strstr(o.text, "not in the widget's reads") != NULL);
	}
	snprintf(home, sizeof home, "%s", saved);
}

static void set_places(const char *content)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/.config/glassine/places", home);
	if (content) {
		FILE *f = fopen(p, "w");
		fputs(content, f);
		fclose(f);
	} else {
		remove(p);
	}
}

/* reads inside {books}: resolved from the places setting before the reads checks */
static void places(void)
{
	char cmd[PATH_MAX * 2];
	snprintf(cmd, sizeof cmd,
		 "cd '%s' && mkdir -p .config/glassine Books Library && echo -n default > Books/a.txt && "
		 "echo -n other > Library/a.txt && ln -s Library LinkLib && ln -s ../secret.txt Library/link && "
		 "ln -s /proc procl", home);
	sh(cmd);
	for (int fb = 0; fb < 2; fb++) {
		set_places(NULL); /* the default, ~/Books */
		struct out o = run(W("books"), NULL, 0, fb, false, NULL);
		CHECK_STR(text_of(&o), "read: default");
		char abs[PATH_MAX + 64];
		snprintf(abs, sizeof abs, "{\"path\":\"%s/Books/a.txt\"}", home);
		o = run(W("books"), abs, 0, fb, false, NULL);
		CHECK_STR(text_of(&o), "read: default");

		set_places("books = \"~/Library\"\n");
		o = run(W("books"), NULL, 0, fb, false, NULL);
		CHECK_STR(text_of(&o), "read: other");
		/* the old library is no longer readable */
		o = run(W("books"), abs, 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		/* links inside the place are not followed */
		o = run(W("books"), "{\"path\":\"{books}/link\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);

		/* unset: the read fails, nothing wider */
		set_places("books = \"\"\n");
		o = run(W("books"), NULL, 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		o = run(W("books"), abs, 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);

		/* a missing folder: no such file */
		set_places("books = \"~/Nowhere\"\n");
		o = run(W("books"), NULL, 0, fb, false, NULL);
		CHECK_STR(text_of(&o), "nil: no such file");

		/* a place that is a link is refused */
		set_places("books = \"~/LinkLib\"\n");
		o = run(W("books"), NULL, 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		CHECK(strstr(o.text, "symlink") != NULL);

		/* a place in a system area is refused, also through a link on the way */
		set_places("books = \"/proc/self\"\n");
		o = run(W("books"), "{\"path\":\"{books}/environ\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		/* ~/procl -> /proc: the place itself is no link, but where it really is decides */
		set_places("books = \"~/procl/sys\"\n");
		o = run(W("books"), "{\"path\":\"{books}/kernel/ostype\"}", 0, fb, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		CHECK(strstr(o.text, "not in the widget's reads") != NULL);
	}
	set_places(NULL);
}

static void data_files(void)
{
	struct out o = run(W("data"), NULL, 0, false, false, NULL);
	CHECK_STR(text_of(&o), "{7=true,list={1=x,2=y},n=3,title=A}");

	/* a data file can't reach anything: no globals at all */
	o = run(W("data"), "{\"file\":\"code.lua\"}", 0, false, false, NULL);
	CHECK(strstr(text_of(&o), "error: ~/data/code.lua:") && strstr(text_of(&o), "global 'os'"));
	o = run(W("data"), "{\"file\":\"load.lua\"}", 0, false, false, NULL);
	CHECK(strstr(text_of(&o), "global 'load'") != NULL);
	o = run(W("data"), "{\"file\":\"strmeta.lua\"}", 0, false, false, NULL);
	CHECK(strstr(text_of(&o), "error: ") != NULL);
	/* precompiled chunks are refused */
	o = run(W("data"), "{\"file\":\"bytecode.lua\"}", 0, false, false, NULL);
	CHECK(strstr(text_of(&o), "binary chunk") != NULL);
	o = run(W("data"), "{\"file\":\"syntax.lua\"}", 0, false, false, NULL);
	CHECK(strstr(text_of(&o), "error: ") != NULL);
	/* functions never cross into the widget */
	o = run(W("data"), "{\"file\":\"funcs.lua\"}", 0, false, false, NULL);
	CHECK_STR(text_of(&o), "{t={y=2},x=1}");
	/* KOReader's dump writes inf, -inf, nan, -nan as bare words */
	o = run(W("data"), "{\"file\":\"inf.lua\"}", 0, false, false, NULL);
	CHECK_STR(text_of(&o), "{a=inf,b=-inf,c=nan,d=nan,e=nan}");
	/* shared and cyclic tables copy as such */
	o = run(W("data"), "{\"file\":\"cyclic.lua\"}", 0, false, false, NULL);
	CHECK_STR(text_of(&o), "{name=c,self=<self>}");
	/* a data file has its own memory cap and the run's CPU budget */
	o = run(W("data"), "{\"file\":\"huge.lua\"}", 0, false, false, NULL);
	CHECK(strstr(text_of(&o), "data file too large") != NULL);
	o = run(W("data"), "{\"file\":\"loop.lua\"}", 0, false, false, NULL);
	CHECK_INT(o.status, RUN_TIME);
	/*
	 * fits in its own 4 MB, but the copy doesn't fit in the widget's: over the memory
	 * limit (the data state used to panic while copying, reported as blocked)
	 */
	o = run(W("data"), "{\"file\":\"many.lua\"}", 0, false, false, NULL);
	CHECK_INT(o.status, RUN_MEMORY);
	CHECK_STR(o.text, "over the memory limit");
	/* too deep for the copy: nil and a reason, as for a broken file */
	o = run(W("data"), "{\"file\":\"deep.lua\"}", 0, false, false, NULL);
	CHECK_STR(text_of(&o), "error: ~/data/deep.lua: data nested too deep");
	/* a data file is a file like any other: unlisted is refused */
	o = run(W("data"), "{\"file\":\"../secret.txt\"}", 0, false, false, NULL);
	CHECK_INT(o.status, RUN_ERROR);
}

static void environment(void)
{
	struct out o = run(W("env"), NULL, 0, false, false, NULL);
	CHECK_STR(text_of(&o), "reachable: ");
	/* widget scripts are loaded as text only */
	char dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s/widgets/binary", home);
	o = run(dir, NULL, 0, false, false, NULL);
	CHECK_INT(o.status, RUN_ERROR);
	CHECK(strstr(o.text, "binary chunk") != NULL);
	o = run(W("norender"), NULL, 0, false, false, NULL);
	CHECK_STR(o.text, "the script must return { render = function(ctx, props) ... end }");

	/*
	 * pairs and next walk keys in one order, whatever Lua's hash seed (it changes every
	 * second): false, true, numbers, strings bytewise, then other keys; a key removed
	 * during a loop is skipped
	 */
	o = run(W("order"), NULL, 0, false, false, NULL);
	const char *all = "false true -2 1.5 3 Bravo alpha charlie delta echo foxtrot golf hotel table";
	char want[512];
	snprintf(want, sizeof want, "[{\"t\":\"text\",\"text\":\"%s\"},{\"t\":\"text\",\"text\":\"%s\"},"
		 "{\"t\":\"text\",\"text\":\"false true -2 1.5 3 Bravo alpha charlie echo foxtrot golf hotel table\"}]",
		 all, all);
	CHECK_STR(o.text, want);
	o = run(W("order"), "{\"case\":\"nextloop\"}", 0, false, false, NULL);
	CHECK_STR(text_of(&o), "20000");
}

static void limits(void)
{
	/* the count hook ends it (RUN_TIME), not the wall backstop (RUN_TIMEOUT) */
	struct out o = run(W("loop"), NULL, 0, false, false, NULL);
	CHECK_INT(o.status, RUN_TIME);
	/* pcall can't catch the time limit */
	o = run(W("pcallloop"), NULL, 0, false, false, NULL);
	CHECK_INT(o.status, RUN_TIME);
	/* a long call into C (a pathological pattern) ends by the CPU timer */
	o = run(W("pattern"), NULL, 0, false, false, NULL);
	CHECK_INT(o.status, RUN_TIME);
	/* memory: one huge string, many small ones, and pcall over the cap */
	o = run(W("rep"), NULL, 0, false, false, NULL);
	CHECK_INT(o.status, RUN_MEMORY);
	o = run(W("table"), NULL, 0, false, false, NULL);
	CHECK_INT(o.status, RUN_MEMORY);
	o = run(W("pcallmem"), NULL, 0, false, false, NULL);
	CHECK_INT(o.status, RUN_MEMORY);
	/* a run that blocks without using CPU: the wall backstop kills it */
	o = run(W("hello"), NULL, HOOK_BLOCK, false, false, NULL);
	CHECK_INT(o.status, RUN_TIMEOUT);
	/*
	 * a run that asks for files and never reads the answers: Home must not block
	 * sending them, or the wall backstop would never run. The alarm turns a hang into
	 * a failure.
	 */
	alarm(60);
	o = run(W("hello"), NULL, HOOK_FLOOD, false, false, NULL);
	alarm(0);
	CHECK_INT(o.status, RUN_CRASH);
	CHECK_STR(o.text, "stopped: it asked for files without reading the answers");
	/* an allocation that fails outside Lua ends the run as memory, not as a blocked system call */
	o = run(W("hello"), NULL, HOOK_OOM, false, false, NULL);
	CHECK_INT(o.status, RUN_MEMORY);
	CHECK_STR(o.text, "over the memory limit");
}

/* real failing runs: the last good output stays, the third failure in a row pauses */
static void pausing(void)
{
	struct widget_state st = { 0 };
	struct out o = run(W("badout"), "{\"case\":\"none\"}", 0, false, false, NULL);
	CHECK_INT(state_record(&st, o.status, o.hash), OUTCOME_CHANGED);
	o = run(W("badout"), "{\"case\":\"error\"}", 0, false, false, NULL);
	CHECK_INT(state_record(&st, o.status, o.hash), OUTCOME_FAILED);
	o = run(W("loop"), NULL, 0, false, false, NULL);
	CHECK_INT(state_record(&st, o.status, o.hash), OUTCOME_FAILED);
	o = run(W("hello"), NULL, HOOK_SOCKET, false, false, NULL);
	CHECK_INT(state_record(&st, o.status, o.hash), OUTCOME_PAUSED);
	CHECK(!state_should_run(&st, TRIGGER_APPEAR));
	CHECK(st.has_output);
}

static void seccomp(void)
{
	static const int hooks[] = { HOOK_SOCKET, HOOK_EXEC, HOOK_OPEN, HOOK_FORK, HOOK_MMAP_EXEC };
	for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++) {
		struct out o = run(W("hello"), NULL, hooks[i], false, false, NULL);
		CHECK_INT(o.status, RUN_BLOCKED);
	}
}

/* Home's side of the boundary: raw messages a compromised run could send */
static void boundary(void)
{
	struct out o = run(W("hello"), NULL, HOOK_RAW_JSON, false, false, NULL);
	CHECK_INT(o.status, RUN_ERROR);
	CHECK_STR(o.text, "invalid output: expected key at byte 1");
	o = run(W("hello"), NULL, HOOK_RAW_DESC, false, false, NULL);
	CHECK_INT(o.status, RUN_ERROR);
	CHECK_STR(o.text, "invalid output: render must return a list of ui primitives");
	/* cut to Home's buffer, "[]" and spaces would pass as JSON: the length decides */
	o = run(W("hello"), NULL, HOOK_RAW_BIG, false, false, NULL);
	CHECK_INT(o.status, RUN_ERROR);
	CHECK_STR(o.text, "invalid output: the description is too large");
}

static void outputs(void)
{
	/* the same inputs give the same bytes */
	struct out a = run(W("hello"), NULL, 0, false, false, NULL);
	struct out b = run(W("hello"), NULL, 0, false, false, NULL);
	CHECK_INT(a.status, RUN_OK);
	CHECK_STR(a.text, "[{\"t\":\"title\",\"text\":\"Hello, world\"},{\"t\":\"meta\",\"text\":\"now\",\"time\":1700000000}]");
	CHECK_STR(a.text, b.text);
	CHECK(a.hash == b.hash && a.hash != 0);
	struct out c = run(W("hello"), "{\"who\":\"you\"}", 0, false, false, NULL);
	CHECK(c.hash != a.hash);

	static const struct { const char *c, *msg; } bad[] = {
		{ "function", "a function can't be part of a description" },
		{ "mixed", "a table in the description mixes list items and named fields" },
		{ "titles", "at most one ui.title" },
		{ "number", "render must return a list of ui primitives" },
		{ "set", "ui.set works only in widget.edit.lua" },
		{ "error", "widget.lua:8: boom" },
		{ "errtable", "error (not a string)" },
		{ "imgfile", "invalid output: ui.image: /etc/hostname is not in the widget's reads" },
		/* 3000 references to one 1 MB string: copied out of Lua with bounds, not in full */
		{ "bigstrings", "the description is too large" },
		/* more items than the walk counts: too large, not mixed */
		{ "biglist", "the description is too large" },
		/* the copy keeps only the start of a long string, but not valid UTF-8 past it still fails */
		{ "badtail", "ui.text: the text is not valid UTF-8" },
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		char props[64];
		snprintf(props, sizeof props, "{\"case\":\"%s\"}", bad[i].c);
		struct out o = run(W("badout"), props, 0, false, false, NULL);
		CHECK_INT(o.status, RUN_ERROR);
		CHECK_STR(o.text, bad[i].msg);
	}

	/*
	 * a long error message and a long print line are cut at a character boundary, on
	 * both sides, not dropped as invalid UTF-8
	 */
	struct out le = run(W("badout"), "{\"case\":\"longerror\"}", 0, false, false, NULL);
	CHECK_INT(le.status, RUN_ERROR);
	CHECK(!strncmp(le.text, "widget.lua:", 11) && strlen(le.text) > 500 && utf8_valid(le.text, strlen(le.text)));
	struct out lp = run(W("badout"), "{\"case\":\"longprint\"}", 0, false, false, NULL);
	CHECK_INT(lp.nlog, 1);
	CHECK_INT(strlen(lp.log0), 298); /* "x" and 99 characters of 3 bytes: the 100th would pass 300 */
	CHECK(utf8_valid(lp.log0, strlen(lp.log0)));

	/* longer text is cut at a character boundary, as before the copy was bounded */
	struct out l = run(W("badout"), "{\"case\":\"longtext\"}", 0, false, false, NULL);
	char want[300] = "[{\"t\":\"text\",\"text\":\"";
	for (int i = 0; i < 100; i++)
		strcat(want, "\u00e9");
	strcat(want, "\"}]");
	CHECK_STR(l.text, want);

	/* a settings script: rows carry ui.set and the current choice */
	const char *apps = "{\"categories\":[{\"name\":\"Games\",\"count\":3},{\"name\":\"Office\",\"count\":5}]}";
	struct out e = run(W("settings"), NULL, 0, false, true, apps);
	CHECK_STR(e.text, "[{\"t\":\"title\",\"text\":\"Show apps from\"},{\"rows\":["
			  "{\"action\":{\"a\":\"set\",\"props\":{\"category\":\"Games\"}},\"label\":\"Games\",\"meta\":\"3 apps\",\"selected\":true},"
			  "{\"action\":{\"a\":\"set\",\"props\":{\"category\":\"Office\"}},\"label\":\"Office\",\"meta\":\"5 apps\"}],\"t\":\"list\"}]");
}

static long long mono_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* the event-loop API: run_start returns at once, run_step finishes it */
static void stepping(void)
{
	char err[512];
	struct manifest m;
	const char *dirs[2] = { W("loop"), W("hello") };
	for (int k = 0; k < 2; k++) {
		CHECK(manifest_load(dirs[k], &m, err, sizeof err));
		struct value *props = props_effective(&m, NULL);
		struct run_opts opts = { .now = 1700000000, .home = home };
		struct run_result r;
		long long t0 = mono_ms();
		struct run *run = run_start(&z, dirs[k], &m, props, &opts, &r);
		CHECK(run != NULL);
		CHECK(mono_ms() - t0 < 150); /* no waiting for the script */
		CHECK(run_fd(run) >= 0);
		int steps = 0;
		while (!run_step(run)) {
			struct pollfd p = { run_fd(run), POLLIN, 0 };
			poll(&p, 1, run_timeout_ms(run));
			steps++;
		}
		CHECK(run_done(run));
		CHECK(run_fd(run) == -1);
		CHECK(steps >= 1);
		CHECK_INT(r.status, k == 0 ? RUN_TIME : RUN_OK);
		run_free(run);
		run_result_free(&r);
		v_free(props);
		manifest_free(&m);
	}
}

/*
 * The zygote dies during a run (an OOM kill): the run ends as a crash at once instead
 * of waiting on the zygote's hung-up socket forever, and later runs fail at once.
 */
static void helper_gone(void)
{
	struct zygote z2;
	CHECK(zygote_start(&z2));
	char err[512];
	struct manifest m;
	CHECK(manifest_load(W("hello"), &m, err, sizeof err));
	struct value *props = props_effective(&m, NULL);
	/* HOOK_BLOCK waits on its socket: only the zygote's death ends it before the wall clock */
	struct run_opts opts = { .now = 1700000000, .home = home, .test_hook = HOOK_BLOCK, .wall_ms = 5000 };
	struct run_result r;
	struct run *run = run_start(&z2, W("hello"), &m, props, &opts, &r);
	CHECK(run != NULL && !run_done(run));
	kill(z2.pid, SIGKILL);
	alarm(10); /* a run_step that never returns ends the suite here */
	while (!run_step(run)) {
		struct pollfd p = { run_fd(run), POLLIN, 0 };
		poll(&p, 1, run_timeout_ms(run));
	}
	alarm(0);
	CHECK(z2.gone);
	CHECK_INT(r.status, RUN_CRASH);
	CHECK_STR(r.msg, "stopped: Home's widget helper stopped; widgets run again when Home restarts");
	run_free(run);
	run_result_free(&r);
	/* the next run doesn't wait: it is done when run_start returns */
	opts.test_hook = 0;
	run = run_start(&z2, W("hello"), &m, props, &opts, &r);
	CHECK(run != NULL && run_done(run));
	CHECK_INT(r.status, RUN_CRASH);
	CHECK_STR(r.msg, "could not start the run: Home's widget helper stopped; widgets run again when Home restarts");
	run_free(run);
	run_result_free(&r);
	v_free(props);
	manifest_free(&m);
	zygote_stop(&z2);
}

int main(void)
{
	if (!zygote_start(&z)) {
		perror("zygote");
		return 2;
	}
	setup_home();
	files();
	home_is_root();
	no_proc();
	places();
	data_files();
	environment();
	limits();
	seccomp();
	pausing();
	boundary();
	outputs();
	stepping();
	helper_gone();
	zygote_stop(&z);
	return t_done("sandbox");
}
