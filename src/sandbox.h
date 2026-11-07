/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Running widget scripts.
 *
 * Home's main process never runs widget code. At startup, before anything else, it
 * forks a zygote; for every run the zygote forks a child that gets one socket and
 * nothing else. The child caps its memory, CPU time and address space, enters a
 * seccomp allowlist, runs one script in a fresh Lua state and sends back the
 * description as canonical JSON. File access goes through the socket: the child asks,
 * Home checks the manifest's reads and passes an open fd (broker.h).
 */
#ifndef GH_SANDBOX_H
#define GH_SANDBOX_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "broker.h"
#include "manifest.h"
#include "value.h"

#define RUN_CPU_MS 200
#define RUN_MEM_BYTES (4 * 1024 * 1024)
#define RUN_WALL_MS 2000
#define RUN_MAX_SCRIPT (128 * 1024)
#define RUN_MAX_LOG 50

/* Test-only payloads, honoured only by a child built with GH_TEST_HOOKS. */
enum {
	HOOK_SOCKET = 1, /* call socket() after the filter is in place */
	HOOK_EXEC = 2,   /* execve /bin/sh */
	HOOK_OPEN = 3,   /* open() a file */
	HOOK_BLOCK = 4,  /* block in recvmsg on the socket: only the wall backstop ends it */
	HOOK_FORK = 5,   /* fork() */
	HOOK_FLOOD = 6,  /* ask for files without reading the answers, then wait without using CPU */
};

struct zygote {
	pid_t pid;
	int fd;
	/* exit statuses that arrived while waiting for something else */
	struct { pid_t pid; int status; } done[16];
	int ndone;
};

/* Forks the zygote. Call first thing in main(), before threads or large allocations. */
bool zygote_start(struct zygote *z);
void zygote_stop(struct zygote *z);

enum run_status {
	RUN_OK,
	RUN_ERROR,      /* the script raised an error or returned an invalid description */
	RUN_TIME,       /* over the CPU budget */
	RUN_MEMORY,     /* over the memory budget */
	RUN_TIMEOUT,    /* the wall-clock backstop killed it */
	RUN_BLOCKED,    /* the seccomp filter killed it (a forbidden system call) */
	RUN_CRASH,      /* died some other way, or stopped reading Home's answers */
};

struct run_opts {
	bool edit;               /* run widget.edit.lua (ui.set and selected allowed) */
	long long now;           /* ctx.now, seconds */
	const struct value *apps;/* ctx.apps data, may be NULL */
	int cpu_ms, wall_ms;     /* 0 = defaults */
	long mem_bytes;          /* 0 = default */
	int test_hook;           /* HOOK_*, test builds only */
	const char *home;        /* expands "~/" in reads; NULL = $HOME */
	bool force_fallback;     /* tests: no openat2 */
};

struct run_result {
	enum run_status status;
	char msg[512];
	struct buf json;         /* canonical description (RUN_OK) */
	uint64_t hash;           /* hash64 of json */
	struct value *desc;      /* normalized description (RUN_OK) */
	char *files[BROKER_MAX_FILES]; /* files the run asked for, for on-change watching */
	size_t nfiles;
	char *log[RUN_MAX_LOG];  /* print() lines */
	size_t nlog;
};

/*
 * Runs DIR/widget.lua (or widget.edit.lua) once with the given settings. Blocks until
 * the run ends; at most wall_ms plus a moment.
 */
void run_widget(struct zygote *z, const char *dir, const struct manifest *m, const struct value *props,
		const struct run_opts *o, struct run_result *r);
void run_result_free(struct run_result *r);
const char *run_status_name(enum run_status s);

/* ---- internal: the wire format between Home, the zygote and the child ---- */

#define REQ_MAGIC 0x676877u /* "ghw" */

struct run_req {
	uint32_t magic;
	uint32_t edit;
	uint32_t hook;
	uint32_t cpu_ms;
	uint64_t mem_bytes;
	int64_t now;
	uint32_t script_len, props_len, apps_len, name_len;
	/* followed by script, props JSON, apps JSON, chunk name */
};

/* child -> Home */
enum { MSG_OPEN = 'O', MSG_PRINT = 'P', MSG_DONE = 'D', MSG_ERROR = 'E' };
/* MSG_ERROR's second byte */
enum { ERR_SCRIPT = 'e', ERR_TIME = 't', ERR_MEMORY = 'm' };
/* Home -> child */
enum { MSG_FD = 'F', MSG_NOFD = 'N' };
/* zygote -> Home */
enum { MSG_SPAWNED = 'S', MSG_EXITED = 'X' };

/* The child's entry point (child.c); never returns. */
void child_main(int sock, const struct run_req *req, size_t len, uint64_t as_limit) __attribute__((noreturn));

/* Installs the seccomp allowlist for a child whose only socket is fd `sock` (seccomp.c). */
bool seccomp_install(int sock);

#endif
