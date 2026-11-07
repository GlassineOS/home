// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * One widget run, inside the sandbox. Forked by the zygote with one socket (fd 3).
 *
 * Order: resource limits, a CPU timer, no-new-privs + the seccomp filter, and only
 * then a Lua state. No stdio here: musl's stdio would make system calls the filter
 * doesn't allow, and exit() would flush it. Raw write/sendmsg and _exit only.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "desc.h"
#include "sandbox.h"

#define MAX_DESC_DEPTH 16
#define MAX_DESC_NODES 4000
/*
 * The returned description is copied out of Lua with bounds, so the copy can't outgrow
 * the run's memory. The normalizer keeps at most 200 bytes of any text and refuses
 * anything longer than 4200 bytes (an image source), so past MAX_DESC_STR bytes only
 * whether the rest of a string is valid UTF-8 can change the outcome.
 */
#define MAX_DESC_STR 8192
#define MAX_DESC_TEXT (256 * 1024) /* all strings and field names together */
#define MAX_DATA_DEPTH 64
#define MAX_DATA_NODES 200000

struct cap {
	size_t used, cap;
	bool hit;
};

static int sock = 3;
static struct cap wcap;         /* the widget's state */
static struct timespec cpu_deadline;
static bool edit_script;
static int nlog;

/* ---- raw I/O ---- */

static void send_msg(const void *p, size_t n)
{
	struct iovec iov = { (void *)p, n };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
	while (sendmsg(sock, &mh, MSG_NOSIGNAL) < 0 && errno == EINTR)
		;
}

static void send_tagged(char tag, char sub, const char *s, size_t n)
{
	char m[1024];
	size_t h = sub ? 2 : 1;
	if (n > sizeof m - h)
		n = sizeof m - h;
	m[0] = tag;
	if (sub)
		m[1] = sub;
	memcpy(m + h, s, n);
	send_msg(m, n + h);
}

static void die(char kind, const char *msg) __attribute__((noreturn));
static void die(char kind, const char *msg)
{
	send_tagged(MSG_ERROR, kind, msg, strlen(msg));
	_exit(kind == ERR_SCRIPT ? 2 : kind == ERR_TIME ? 4 : 3);
}

/* value.c's allocation failure: over the memory limit, not abort() (which the filter forbids) */
static void out_of_memory(void)
{
	die(ERR_MEMORY, "over the memory limit");
}

/* ---- limits ---- */

static void *capped_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	struct cap *c = ud;
	size_t old = ptr ? osize : 0;
	if (nsize == 0) {
		free(ptr);
		c->used -= old;
		return NULL;
	}
	if (nsize > old && c->used + (nsize - old) > c->cap) {
		c->hit = true;
		return NULL;
	}
	void *p = realloc(ptr, nsize);
	if (!p) {
		c->hit = true;
		return NULL;
	}
	c->used = c->used - old + nsize;
	return p;
}

static bool past_deadline(void)
{
	struct timespec t;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return t.tv_sec > cpu_deadline.tv_sec || (t.tv_sec == cpu_deadline.tv_sec && t.tv_nsec >= cpu_deadline.tv_nsec);
}

/*
 * Every 1000 instructions. Over the CPU budget or after the memory cap was hit, the
 * run ends here: raising an error would let pcall catch it and go on.
 */
static void hook(lua_State *L, lua_Debug *ar)
{
	(void)L;
	(void)ar;
	if (wcap.hit)
		die(ERR_MEMORY, "over the memory limit");
	if (past_deadline())
		die(ERR_TIME, "over the time limit");
}

static void set_limits(uint64_t as_limit, unsigned cpu_ms)
{
	struct rlimit r;
	r.rlim_cur = r.rlim_max = as_limit;
	setrlimit(RLIMIT_AS, &r);
	r.rlim_cur = r.rlim_max = 0;
	setrlimit(RLIMIT_NPROC, &r);
	setrlimit(RLIMIT_FSIZE, &r);
	setrlimit(RLIMIT_CORE, &r);
	r.rlim_cur = r.rlim_max = 16;
	setrlimit(RLIMIT_NOFILE, &r);
	r.rlim_cur = 1;
	r.rlim_max = 2;
	setrlimit(RLIMIT_CPU, &r);
	/*
	 * A long call into C (string.rep, a bad pattern) never reaches the count hook. The
	 * profiling timer's default action ends the process a little after the budget.
	 */
	unsigned ms = cpu_ms + 100;
	struct itimerval it = { { 0, 0 }, { ms / 1000, (long)(ms % 1000) * 1000 } };
	setitimer(ITIMER_PROF, &it, NULL);
	prctl(PR_SET_DUMPABLE, 0);
}

/* ---- file access through Home ---- */

/* Asks Home to open path; returns an fd, or -1 with a reason in why (and denied set). */
static int request_fd(const char *path, long long *size, char *why, size_t whylen, bool *denied)
{
	size_t n = strlen(path);
	if (n > 4000) {
		snprintf(why, whylen, "path too long");
		*denied = true;
		return -1;
	}
	char m[4096];
	m[0] = MSG_OPEN;
	memcpy(m + 1, path, n);
	send_msg(m, n + 1);

	char reply[512];
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { reply, sizeof reply - 1 };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
	ssize_t r;
	while ((r = recvmsg(sock, &mh, 0)) < 0 && errno == EINTR)
		;
	if (r <= 0)
		_exit(5);
	int fd = -1;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
			memcpy(&fd, CMSG_DATA(c), sizeof fd);
	reply[r] = 0;
	if (reply[0] == MSG_FD && fd >= 0 && r >= 1 + (ssize_t)sizeof(long long)) {
		memcpy(size, reply + 1, sizeof *size);
		return fd;
	}
	if (fd >= 0)
		close(fd);
	*denied = r >= 2 && reply[1] == 'd';
	snprintf(why, whylen, "%s", r >= 3 ? reply + 2 : "refused");
	return -1;
}

/* Reads an fd from Home fully into a malloc'd buffer (at most the size Home reported). */
static char *slurp(int fd, long long size, size_t *len)
{
	size_t cap = (size_t)size + 1, n = 0;
	char *b = malloc(cap);
	if (!b) {
		close(fd);
		return NULL;
	}
	for (;;) {
		if (n == cap - 1)
			break; /* the file grew: take what Home measured */
		ssize_t r = read(fd, b + n, cap - 1 - n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		n += (size_t)r;
	}
	close(fd);
	b[n] = 0;
	*len = n;
	return b;
}

static char *fetch(lua_State *L, const char *path, size_t *len, char *why, size_t whylen)
{
	long long size = 0;
	bool denied = false;
	int fd = request_fd(path, &size, why, whylen, &denied);
	if (fd < 0) {
		if (denied)
			luaL_error(L, "%s: %s", path, why);
		return NULL;
	}
	char *b = slurp(fd, size, len);
	if (!b)
		luaL_error(L, "not enough memory");
	return b;
}

/* ---- ctx.read, ctx.load_lua, ctx.sidecar ---- */

static int l_read(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	char why[256];
	size_t len;
	char *b = fetch(L, path, &len, why, sizeof why);
	if (!b) {
		lua_pushnil(L);
		lua_pushstring(L, why);
		return 2;
	}
	lua_pushlstring(L, b, len); /* counted against the widget's memory */
	free(b);
	return 1;
}

struct copy {
	lua_State *src, *dst;
	int seen_src, seen_dst; /* absolute indices of the table maps */
	long nodes;
	int next_id;
	const char *err;
};

static bool copy_value(struct copy *c, int si, int depth);

static bool copy_key_ok(lua_State *L, int idx)
{
	int t = lua_type(L, idx);
	return t == LUA_TSTRING || t == LUA_TNUMBER || t == LUA_TBOOLEAN;
}

/* Pushes onto dst a copy of the value at src index si (scalars and tables only; others become nil). */
static bool copy_value(struct copy *c, int si, int depth)
{
	lua_State *s = c->src, *d = c->dst;
	if (!lua_checkstack(s, 4) || !lua_checkstack(d, 6)) {
		c->err = "data nested too deep";
		return false;
	}
	if (++c->nodes > MAX_DATA_NODES) {
		c->err = "data file too large";
		return false;
	}
	switch (lua_type(s, si)) {
	case LUA_TBOOLEAN:
		lua_pushboolean(d, lua_toboolean(s, si));
		return true;
	case LUA_TNUMBER:
		if (lua_isinteger(s, si))
			lua_pushinteger(d, lua_tointeger(s, si));
		else
			lua_pushnumber(d, lua_tonumber(s, si));
		return true;
	case LUA_TSTRING: {
		size_t n;
		const char *p = lua_tolstring(s, si, &n);
		lua_pushlstring(d, p, n);
		return true;
	}
	case LUA_TTABLE:
		break;
	default:
		lua_pushnil(d); /* functions and the like never cross over */
		return true;
	}
	if (depth > MAX_DATA_DEPTH) {
		c->err = "data nested too deep";
		return false;
	}
	si = lua_absindex(s, si);
	/* a table seen before (shared or cyclic) maps to the same copy */
	lua_pushvalue(s, si);
	if (lua_rawget(s, c->seen_src) == LUA_TNUMBER) {
		lua_Integer id = lua_tointeger(s, -1);
		lua_pop(s, 1);
		lua_rawgeti(d, c->seen_dst, id);
		return true;
	}
	lua_pop(s, 1);
	int id = ++c->next_id;
	lua_pushvalue(s, si);
	lua_pushinteger(s, id);
	lua_rawset(s, c->seen_src);
	lua_newtable(d);
	int di = lua_gettop(d);
	lua_pushvalue(d, di);
	lua_rawseti(d, c->seen_dst, id);
	lua_pushnil(s);
	while (lua_next(s, si)) { /* raw traversal: no metatables exist in the data state */
		if (copy_key_ok(s, -2)) {
			if (!copy_value(c, -2, depth + 1) || !copy_value(c, -1, depth + 1))
				return false;
			if (lua_isnil(d, -1) || lua_isnil(d, -2) || (lua_type(d, -2) == LUA_TNUMBER && isnan(lua_tonumber(d, -2))))
				lua_pop(d, 2);
			else
				lua_rawset(d, di);
		}
		lua_pop(s, 1);
	}
	return true;
}

static struct cap dcap;

static void data_hook(lua_State *L, lua_Debug *ar)
{
	(void)L;
	(void)ar;
	if (past_deadline())
		die(ERR_TIME, "over the time limit (reading a data file)");
}

static int l_load_lua(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	char why[256];
	size_t len;
	char *b = fetch(L, path, &len, why, sizeof why);
	if (!b) {
		lua_pushnil(L);
		lua_pushstring(L, why);
		return 2;
	}
	/* a bare state: no libraries, no string metatable, nothing to reach */
	dcap = (struct cap){ 0, RUN_MEM_BYTES, false };
	lua_State *D = lua_newstate(capped_alloc, &dcap);
	if (!D) {
		free(b);
		return luaL_error(L, "not enough memory");
	}
	lua_sethook(D, data_hook, LUA_MASKCOUNT, 1000);
	/* KOReader writes infinities and NaN with tostring(): inf, -inf, nan, -nan */
	lua_newtable(D);
	lua_pushnumber(D, HUGE_VAL);
	lua_setfield(D, -2, "inf");
	lua_pushnumber(D, NAN);
	lua_setfield(D, -2, "nan");
	int env = lua_gettop(D);
	int st = luaL_loadbufferx(D, b, len, "=data", "t");
	free(b);
	if (st == LUA_OK) {
		lua_pushvalue(D, env);
		lua_setupvalue(D, -2, 1); /* _ENV */
		st = lua_pcall(D, 0, 1, 0);
	}
	if (st != LUA_OK) {
		const char *e = dcap.hit ? "data file too large" : lua_tostring(D, -1);
		lua_pushnil(L);
		lua_pushfstring(L, "%s: %s", path, e ? e : "error");
		lua_close(D);
		return 2;
	}
	lua_newtable(D);
	int seen_src = lua_gettop(D);
	lua_newtable(L);
	int seen_dst = lua_gettop(L);
	struct copy c = { D, L, seen_src, seen_dst, 0, 0, NULL };
	if (!copy_value(&c, -2, 0)) {
		const char *e = c.err;
		lua_close(D);
		return luaL_error(L, "%s: %s", path, e);
	}
	lua_close(D);
	lua_remove(L, seen_dst);
	return 1;
}

/* ctx.sidecar(book): KOReader keeps a book's metadata in BOOK-without-extension.sdr/metadata.EXT.lua */
static int l_sidecar(lua_State *L)
{
	size_t n;
	const char *book = luaL_checklstring(L, 1, &n);
	const char *slash = strrchr(book, '/');
	const char *dot = strrchr(book, '.');
	if (!slash || !dot || dot < slash || dot == slash + 1 || !dot[1] || n > 4000)
		return luaL_error(L, "ctx.sidecar: not a book path: %s", book);
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	luaL_addlstring(&b, book, (size_t)(dot - book));
	luaL_addstring(&b, ".sdr/metadata.");
	for (const char *e = dot + 1; *e; e++)
		luaL_addchar(&b, (char)((*e >= 'A' && *e <= 'Z') ? *e - 'A' + 'a' : *e));
	luaL_addstring(&b, ".lua");
	luaL_pushresult(&b);
	return 1;
}

static int l_print(lua_State *L)
{
	if (nlog >= RUN_MAX_LOG)
		return 0;
	nlog++;
	int n = lua_gettop(L);
	luaL_checkstack(L, 2 * n + 1, "too many arguments");
	for (int i = 1; i <= n; i++) {
		if (i > 1)
			lua_pushliteral(L, "\t");
		luaL_tolstring(L, i, NULL);
	}
	lua_concat(L, n ? 2 * n - 1 : 0);
	size_t len;
	const char *s = lua_tolstring(L, -1, &len);
	send_tagged(MSG_PRINT, 0, s, len > 300 ? 300 : len);
	return 0;
}

/* ---- value <-> Lua ---- */

static void push_value(lua_State *L, const struct value *v)
{
	luaL_checkstack(L, 4, "nested too deep");
	switch (v->t) {
	case V_NULL: lua_pushnil(L); break;
	case V_BOOL: lua_pushboolean(L, v->u.b); break;
	case V_INT: lua_pushinteger(L, v->u.i); break;
	case V_FLOAT: lua_pushnumber(L, v->u.f); break;
	case V_STR: lua_pushlstring(L, v->u.s.p, v->u.s.len); break;
	case V_ARR:
		lua_createtable(L, (int)v->u.a.n, 0);
		for (size_t i = 0; i < v->u.a.n; i++) {
			push_value(L, v->u.a.v[i]);
			lua_rawseti(L, -2, (lua_Integer)i + 1);
		}
		break;
	case V_MAP:
		lua_createtable(L, 0, (int)v->u.m.n);
		for (size_t i = 0; i < v->u.m.n; i++) {
			push_value(L, v->u.m.v[i]);
			lua_setfield(L, -2, v->u.m.k[i]);
		}
		break;
	}
}

struct conv {
	long nodes;
	size_t text; /* bytes of strings and field names copied */
	char err[200];
};

static struct value *copy_str(const char *s, size_t n, struct conv *c)
{
	size_t keep = n;
	if (n > MAX_DESC_STR)
		keep = utf8_cut(s, n, MAX_DESC_STR);
	c->text += keep + 1;
	if (c->text > MAX_DESC_TEXT) {
		snprintf(c->err, sizeof c->err, "the description is too large");
		return NULL;
	}
	if (keep == n || utf8_valid(s + keep, n - keep))
		return v_strn(s, keep);
	/* the cut-off rest isn't valid UTF-8: one invalid byte keeps it so */
	struct value *v = v_strn(s, keep + 1);
	v->u.s.p[keep] = (char)0xff;
	return v;
}

/* Raw walk of the returned description: no metamethods, bounded depth and size. */
static struct value *to_value(lua_State *L, int idx, int depth, struct conv *c)
{
	if (++c->nodes > MAX_DESC_NODES) {
		snprintf(c->err, sizeof c->err, "the description is too large");
		return NULL;
	}
	idx = lua_absindex(L, idx);
	switch (lua_type(L, idx)) {
	case LUA_TNIL: return v_null();
	case LUA_TBOOLEAN: return v_bool(lua_toboolean(L, idx));
	case LUA_TNUMBER:
		if (lua_isinteger(L, idx))
			return v_int(lua_tointeger(L, idx));
		return v_float(lua_tonumber(L, idx));
	case LUA_TSTRING: {
		size_t n;
		const char *s = lua_tolstring(L, idx, &n);
		return copy_str(s, n, c);
	}
	case LUA_TTABLE:
		break;
	default:
		snprintf(c->err, sizeof c->err, "a %s can't be part of a description", luaL_typename(L, idx));
		return NULL;
	}
	if (depth > MAX_DESC_DEPTH) {
		snprintf(c->err, sizeof c->err, "the description is nested too deep");
		return NULL;
	}
	luaL_checkstack(L, 4, "nested too deep");
	lua_Unsigned len = lua_rawlen(L, idx);
	long count = 0;
	bool strkeys = true;
	lua_pushnil(L);
	while (lua_next(L, idx)) {
		count++;
		if (lua_type(L, -2) != LUA_TSTRING)
			strkeys = false;
		lua_pop(L, 1);
		if (count > MAX_DESC_NODES)
			break;
	}
	if (len > 0 && (lua_Unsigned)count == len) {
		struct value *a = v_arr();
		for (lua_Unsigned i = 1; i <= len; i++) {
			lua_rawgeti(L, idx, (lua_Integer)i);
			struct value *x = to_value(L, -1, depth + 1, c);
			lua_pop(L, 1);
			if (!x) {
				v_free(a);
				return NULL;
			}
			v_arr_push(a, x);
		}
		return a;
	}
	if (!strkeys) {
		snprintf(c->err, sizeof c->err, "a table in the description mixes list items and named fields");
		return NULL;
	}
	if (count == 0)
		return v_arr();
	struct value *m = v_map();
	lua_pushnil(L);
	while (lua_next(L, idx)) {
		size_t kn;
		const char *k = lua_tolstring(L, -2, &kn);
		if (strlen(k) != kn || kn > 64) {
			lua_pop(L, 2);
			snprintf(c->err, sizeof c->err, "a field name in the description is not valid");
			v_free(m);
			return NULL;
		}
		c->text += kn + 1;
		struct value *x = to_value(L, -1, depth + 1, c);
		lua_pop(L, 1);
		if (!x) {
			lua_pop(L, 1);
			v_free(m);
			return NULL;
		}
		v_map_set(m, k, x);
	}
	return m;
}

/* ---- the ui table, the environment ---- */

static const char prelude[] =
	"local ui = {}\n"
	"local function str(v, what) if type(v) ~= 'string' then error(what .. ' must be a string', 3) end return v end\n"
	"local function opt(t) if t ~= nil and type(t) ~= 'table' then error('options must be a table', 3) end return t or {} end\n"
	"function ui.title(text) return { t = 'title', text = str(text, 'ui.title: the text') } end\n"
	"function ui.text(text, o) o = opt(o) return { t = 'text', text = str(text, 'ui.text: the text'), soft = o.soft and true or nil } end\n"
	"function ui.progress(value, label)\n"
	"  if type(value) ~= 'number' then error('ui.progress needs a number from 0 to 1', 2) end\n"
	"  return { t = 'progress', value = value, label = label }\n"
	"end\n"
	"function ui.list(rows)\n"
	"  if type(rows) ~= 'table' then error('ui.list needs a list of rows', 2) end\n"
	"  local out = {}\n"
	"  for i, r in ipairs(rows) do\n"
	"    if i > 4 then break end\n"
	"    if type(r) ~= 'table' then error('ui.list: each row is { label, meta }', 2) end\n"
	"    out[i] = { label = r[1] or r.label, meta = r[2] or r.meta, selected = r.selected, action = r.action }\n"
	"  end\n"
	"  return { t = 'list', rows = out }\n"
	"end\n"
	"function ui.button(label, icon, action, o) o = opt(o)\n"
	"  return { t = 'button', label = str(label, 'ui.button: the label'), icon = icon, action = action, selected = o.selected }\n"
	"end\n"
	"function ui.meta(text, o) o = opt(o) return { t = 'meta', text = str(text, 'ui.meta: the text'), time = o.time } end\n"
	"function ui.image(s)\n"
	"  if type(s) ~= 'table' then error('ui.image needs { src, w, h, side, fit, alt }', 2) end\n"
	"  return { t = 'image', src = s.src, w = s.w, h = s.h, side = s.side, fit = s.fit, alt = s.alt }\n"
	"end\n"
	"function ui.open(app, file) return { a = 'open', app = str(app, 'ui.open: the app'), file = file } end\n"
	"function ui.settings(page) return { a = 'settings', page = str(page, 'ui.settings: the page') } end\n"
	"function ui.set(props) if type(props) ~= 'table' then error('ui.set needs a table of settings', 2) end\n"
	"  local p = {} for k, v in pairs(props) do p[k] = v end return { a = 'set', props = p } end\n"
	"return ui\n";

static const char *const whitelist[] = {
	"ipairs", "pairs", "next", "type", "tostring", "tonumber", "select", "pcall", "error", "assert", NULL
};

static void build_env(lua_State *L, const struct value *props, const struct value *apps, long long now)
{
	luaL_requiref(L, "_G", luaopen_base, 1);
	int G = lua_gettop(L);
	luaL_requiref(L, "string", luaopen_string, 1);
	/* the string metatable's __index is this same table: remove dump from it */
	lua_pushnil(L);
	lua_setfield(L, -2, "dump");
	lua_pop(L, 1);
	luaL_requiref(L, "table", luaopen_table, 1);
	lua_pop(L, 1);
	luaL_requiref(L, "math", luaopen_math, 1);
	/* deterministic output: same inputs, same bytes */
	lua_pushnil(L);
	lua_setfield(L, -2, "random");
	lua_pushnil(L);
	lua_setfield(L, -2, "randomseed");
	lua_pop(L, 1);
	luaL_requiref(L, "utf8", luaopen_utf8, 1);
	lua_pop(L, 1);

	/* the prelude's own environment */
	lua_newtable(L);
	int penv = lua_gettop(L);
	static const char *const pnames[] = { "type", "error", "ipairs", "pairs", NULL };
	for (const char *const *n = pnames; *n; n++) {
		lua_getfield(L, G, *n);
		lua_setfield(L, penv, *n);
	}
	if (luaL_loadbufferx(L, prelude, sizeof prelude - 1, "=ui", "t") != LUA_OK)
		die(ERR_SCRIPT, "internal: ui prelude");
	lua_pushvalue(L, penv);
	lua_setupvalue(L, -2, 1);
	lua_call(L, 0, 1);
	int ui = lua_gettop(L);

	/* the widget's environment: a whitelist, ui, and nothing else */
	lua_newtable(L);
	int env = lua_gettop(L);
	for (const char *const *n = whitelist; *n; n++) {
		lua_getfield(L, G, *n);
		lua_setfield(L, env, *n);
	}
	static const char *const libs[] = { "string", "table", "math", "utf8", NULL };
	for (const char *const *n = libs; *n; n++) {
		lua_getfield(L, G, *n);
		lua_setfield(L, env, *n);
	}
	lua_pushcfunction(L, l_print);
	lua_setfield(L, env, "print");
	lua_pushvalue(L, ui);
	lua_setfield(L, env, "ui");

	/* ctx and props are arguments to render, kept in the registry */
	lua_newtable(L);
	lua_pushcfunction(L, l_read);
	lua_setfield(L, -2, "read");
	lua_pushcfunction(L, l_load_lua);
	lua_setfield(L, -2, "load_lua");
	lua_pushcfunction(L, l_sidecar);
	lua_setfield(L, -2, "sidecar");
	lua_pushinteger(L, now);
	lua_setfield(L, -2, "now");
	if (apps) {
		push_value(L, apps);
		lua_setfield(L, -2, "apps");
	}
	lua_setfield(L, LUA_REGISTRYINDEX, "gh.ctx");
	push_value(L, props);
	lua_setfield(L, LUA_REGISTRYINDEX, "gh.props");

	lua_pushvalue(L, env);
	lua_setfield(L, LUA_REGISTRYINDEX, "gh.env");
	lua_settop(L, 0);
	/* the real globals stay unreachable: the widget's chunk gets env as _ENV */
}

/* An error outside any protected call, such as running out of memory while building the environment */
static int panic(lua_State *L)
{
	if (wcap.hit)
		die(ERR_MEMORY, "over the memory limit");
	die(ERR_SCRIPT, lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "error");
}

static int msgh(lua_State *L)
{
	if (!lua_isstring(L, 1))
		lua_pushliteral(L, "error (not a string)");
	return 1;
}

#ifdef GH_TEST_HOOKS
#include <linux/futex.h>
#include <sys/syscall.h>
static void test_hook(int hook)
{
	switch (hook) {
	case HOOK_SOCKET: socket(AF_INET, SOCK_STREAM, 0); break;
	case HOOK_EXEC: { char *argv[] = { "sh", NULL }; execve("/bin/sh", argv, argv + 1); break; }
	case HOOK_OPEN: open("/etc/hostname", O_RDONLY); break;
	case HOOK_BLOCK: { char c; struct iovec iov = { &c, 1 }; struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
		recvmsg(sock, &mh, 0); break; }
	case HOOK_FORK: fork(); break;
	case HOOK_OOM: v_strn("", (size_t)1 << 40); break; /* fails under RLIMIT_AS before it copies */
	case HOOK_FLOOD: {
		static const char m[] = "O/nonexistent";
		struct iovec iov = { (void *)m, sizeof m - 1 };
		struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
		for (int i = 0; i < 5000; i++)
			sendmsg(sock, &mh, MSG_NOSIGNAL);
		int w = 0;
		syscall(SYS_futex, &w, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
		break;
	}
	default: break;
	}
}
#endif

void child_main(int s, const struct run_req *req, size_t len, uint64_t as_limit)
{
	sock = s;
	v_oom = out_of_memory;
	if (req->magic != REQ_MAGIC ||
	    len < sizeof *req + (size_t)req->script_len + req->props_len + req->apps_len + req->name_len)
		_exit(6);
	const char *script = (const char *)(req + 1);
	const char *props_json = script + req->script_len;
	const char *apps_json = props_json + req->props_len;
	const char *name = apps_json + req->apps_len;
	char chunk[80];
	snprintf(chunk, sizeof chunk, "=%.*s", (int)(req->name_len < 70 ? req->name_len : 70), name);
	edit_script = req->edit;

	/* parse the inputs before the filter: plain malloc, no system calls beyond memory */
	struct value *props = v_from_json(props_json, req->props_len, 8, NULL, 0);
	struct value *apps = req->apps_len ? v_from_json(apps_json, req->apps_len, 8, NULL, 0) : NULL;
	if (!props)
		_exit(6);

	set_limits(as_limit, req->cpu_ms);
	if (!seccomp_install(sock))
		_exit(7);
#ifdef GH_TEST_HOOKS
	test_hook((int)req->hook);
#endif

	struct timespec t;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	cpu_deadline.tv_sec = t.tv_sec + req->cpu_ms / 1000;
	cpu_deadline.tv_nsec = t.tv_nsec + (long)(req->cpu_ms % 1000) * 1000000L;
	if (cpu_deadline.tv_nsec >= 1000000000L) {
		cpu_deadline.tv_sec++;
		cpu_deadline.tv_nsec -= 1000000000L;
	}

	wcap = (struct cap){ 0, req->mem_bytes ? req->mem_bytes : RUN_MEM_BYTES, false };
	lua_State *L = lua_newstate(capped_alloc, &wcap);
	if (!L)
		die(ERR_MEMORY, "over the memory limit");
	lua_atpanic(L, panic);
	lua_sethook(L, hook, LUA_MASKCOUNT, 1000);
	build_env(L, props, apps, req->now);
	v_free(props);
	v_free(apps);

	lua_pushcfunction(L, msgh);
	int h = lua_gettop(L);
	int st = luaL_loadbufferx(L, script, req->script_len, chunk, "t");
	if (st == LUA_OK) {
		lua_getfield(L, LUA_REGISTRYINDEX, "gh.env");
		lua_setupvalue(L, -2, 1);
		st = lua_pcall(L, 0, 1, h);
	}
	if (st == LUA_OK) {
		if (!lua_istable(L, -1)) {
			die(ERR_SCRIPT, "the script must return { render = function(ctx, props) ... end }");
		}
		lua_pushliteral(L, "render");
		lua_rawget(L, -2);
		if (lua_type(L, -1) != LUA_TFUNCTION)
			die(ERR_SCRIPT, "the script must return { render = function(ctx, props) ... end }");
		lua_getfield(L, LUA_REGISTRYINDEX, "gh.ctx");
		lua_getfield(L, LUA_REGISTRYINDEX, "gh.props");
		st = lua_pcall(L, 2, 1, h);
	}
	if (wcap.hit)
		die(ERR_MEMORY, "over the memory limit");
	if (st != LUA_OK) {
		const char *e = lua_tostring(L, -1);
		die(ERR_SCRIPT, e ? e : "error");
	}

	struct conv c = { 0, 0, "" };
	struct value *v = to_value(L, -1, 0, &c);
	if (!v)
		die(ERR_SCRIPT, c.err);
	char err[256];
	struct value *n = desc_normalize(v, edit_script, NULL, NULL, err, sizeof err);
	v_free(v);
	if (!n)
		die(ERR_SCRIPT, err);
	struct buf b = { 0 };
	buf_add(&b, "D", 1);
	v_to_json(n, &b);
	if (b.len > DESC_MAX_JSON)
		die(ERR_SCRIPT, "the description is too large");
	send_msg(b.p, b.len);
	_exit(0);
}
