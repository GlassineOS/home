// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Home's side of a run: read the script, hand it to the zygote with one end of a
 * fresh socket, serve the child's file requests, enforce the wall-clock backstop, and
 * re-check whatever the child sends back (it is untrusted).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "desc.h"
#include "sandbox.h"

const char *run_status_name(enum run_status s)
{
	static const char *const n[] = { "ok", "error", "time", "memory", "timeout", "blocked", "crash" };
	return (unsigned)s < sizeof n / sizeof *n ? n[s] : "?";
}

void run_result_free(struct run_result *r)
{
	buf_free(&r->json);
	v_free(r->desc);
	for (size_t i = 0; i < r->nfiles; i++)
		free(r->files[i]);
	for (size_t i = 0; i < r->nlog; i++)
		free(r->log[i]);
	memset(r, 0, sizeof *r);
}

static long long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* snprintf, cut at a character boundary: a message cut inside one isn't valid UTF-8 */
static void set_msg(char *dst, size_t dstlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_msg(char *dst, size_t dstlen, const char *fmt, ...)
{
	char tmp[8192];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	size_t len = n < 0 ? 0 : (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1;
	len = utf8_cut(tmp, len, dstlen - 1);
	memcpy(dst, tmp, len);
	dst[len] = 0;
}

static char *read_script(const char *path, size_t *len, char *err, size_t errlen)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > RUN_MAX_SCRIPT) {
		set_msg(err, errlen, "%s: %s", path, fd < 0 ? strerror(errno) : "not a regular file of at most 128 KiB");
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	char *b = malloc((size_t)st.st_size + 1);
	ssize_t n = b ? read(fd, b, (size_t)st.st_size) : -1;
	close(fd);
	if (n != st.st_size) {
		free(b);
		set_msg(err, errlen, "%s: read failed", path);
		return NULL;
	}
	*len = (size_t)n;
	return b;
}

/* Receives one zygote message; MSG_EXITED for other pids are kept for later. */
static bool zygote_recv(struct zygote *z, char *type, pid_t *pid, int *status, int timeout_ms)
{
	struct pollfd p = { z->fd, POLLIN, 0 };
	if (poll(&p, 1, timeout_ms) <= 0)
		return false;
	char m[64];
	ssize_t n = recv(z->fd, m, sizeof m, 0);
	if (n < 1 + (ssize_t)sizeof(pid_t))
		return false;
	*type = m[0];
	memcpy(pid, m + 1, sizeof *pid);
	if (m[0] == MSG_EXITED && n >= 1 + (ssize_t)(sizeof(pid_t) + sizeof(int)))
		memcpy(status, m + 1 + sizeof(pid_t), sizeof *status);
	return true;
}

static pid_t spawn(struct zygote *z, const void *req, size_t len, int child_end)
{
	char cbuf[CMSG_SPACE(sizeof(int))];
	memset(cbuf, 0, sizeof cbuf);
	struct iovec iov = { (void *)req, len };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
	struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &child_end, sizeof child_end);
	if (sendmsg(z->fd, &mh, MSG_NOSIGNAL) != (ssize_t)len)
		return -1;
	for (;;) {
		char type;
		pid_t pid;
		int st = 0;
		if (!zygote_recv(z, &type, &pid, &st, 5000))
			return -1;
		if (type == MSG_SPAWNED)
			return pid;
		if (type == MSG_EXITED && z->ndone < (int)(sizeof z->done / sizeof *z->done)) {
			z->done[z->ndone].pid = pid;
			z->done[z->ndone++].status = st;
		}
	}
}

static void reply_fd(int s, struct broker *b, const char *path)
{
	long long size = 0;
	int fd = broker_open(b, path, &size);
	if (fd >= 0) {
		char m[1 + sizeof size];
		m[0] = MSG_FD;
		memcpy(m + 1, &size, sizeof size);
		char cbuf[CMSG_SPACE(sizeof(int))];
		memset(cbuf, 0, sizeof cbuf);
		struct iovec iov = { m, sizeof m };
		struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
		struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(c), &fd, sizeof fd);
		sendmsg(s, &mh, MSG_NOSIGNAL);
		close(fd);
		return;
	}
	int e = errno;
	char m[300];
	bool denied = e == EACCES || e == EINVAL || e == EMFILE || e == EFBIG;
	const char *why = e == EACCES ? "not in the widget's reads (or a symlink on the way)"
			: e == EINVAL ? "not a regular file or not a valid path"
			: e == EFBIG ? "larger than 2 MB"
			: e == EMFILE ? "too many files in one run"
			: e == ENOENT ? "no such file"
			: strerror(e);
	int n = snprintf(m, sizeof m, "%c%c%s", MSG_NOFD, denied ? 'd' : 'n', why);
	send(s, m, (size_t)n, MSG_NOSIGNAL);
}

struct run {
	struct zygote *z;
	struct run_result *r;
	enum { ST_RUNNING, ST_REAPING, ST_DONE } state;
	pid_t pid;
	int sock;
	long long start, reap_end;
	int wall;
	bool killed, got_final, kill_sent;
	bool edit;
	struct broker b;
	char *msg;
	int status;
};

struct run *run_start(struct zygote *z, const char *dir, const struct manifest *m, const struct value *props,
		      const struct run_opts *o, struct run_result *r)
{
	memset(r, 0, sizeof *r);
	r->status = RUN_CRASH;
	struct run *run = calloc(1, sizeof *run);
	if (!run) {
		snprintf(r->msg, sizeof r->msg, "out of memory");
		return NULL;
	}
	run->z = z;
	run->r = r;
	run->sock = -1;
	run->state = ST_DONE;
	run->edit = o->edit;
	const char *name = o->edit ? "widget.edit.lua" : "widget.lua";
	char path[4096];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	size_t slen = 0;
	char *script = read_script(path, &slen, r->msg, sizeof r->msg);
	if (!script) {
		r->status = RUN_ERROR;
		return run;
	}
	struct buf pj = { 0 }, aj = { 0 }, req = { 0 };
	v_to_json(props, &pj);
	if (o->apps)
		v_to_json(o->apps, &aj);
	struct run_req h = {
		.magic = REQ_MAGIC, .edit = o->edit, .hook = (uint32_t)o->test_hook,
		.cpu_ms = (uint32_t)(o->cpu_ms ? o->cpu_ms : RUN_CPU_MS),
		.mem_bytes = (uint64_t)(o->mem_bytes ? o->mem_bytes : RUN_MEM_BYTES),
		.now = o->now, .script_len = (uint32_t)slen, .props_len = (uint32_t)pj.len,
		.apps_len = (uint32_t)aj.len, .name_len = (uint32_t)strlen(name),
	};
	buf_add(&req, &h, sizeof h);
	buf_add(&req, script, slen);
	buf_add(&req, pj.p, pj.len);
	if (aj.len)
		buf_add(&req, aj.p, aj.len);
	buf_add(&req, name, strlen(name));
	free(script);
	buf_free(&pj);
	buf_free(&aj);

	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
		snprintf(r->msg, sizeof r->msg, "socketpair: %s", strerror(errno));
		buf_free(&req);
		return run;
	}
	run->start = now_ms();
	run->pid = spawn(z, req.p, req.len, sv[1]);
	close(sv[1]);
	buf_free(&req);
	if (run->pid <= 0) {
		close(sv[0]);
		snprintf(r->msg, sizeof r->msg, "could not start the run");
		return run;
	}
	run->sock = sv[0];
	broker_init(&run->b, m, o->home);
	run->b.force_fallback = o->force_fallback;
	run->wall = o->wall_ms ? o->wall_ms : RUN_WALL_MS;
	run->msg = malloc(DESC_MAX_JSON + 16);
	run->state = ST_RUNNING;
	return run;
}

int run_fd(const struct run *run)
{
	if (!run)
		return -1;
	return run->state == ST_RUNNING ? run->sock : run->state == ST_REAPING ? run->z->fd : -1;
}

int run_timeout_ms(const struct run *run)
{
	if (!run || run->state == ST_DONE)
		return 0;
	long long end = run->state == ST_RUNNING ? run->start + run->wall : run->reap_end;
	long long left = end - now_ms();
	return left < 0 ? 0 : left > 60000 ? 60000 : (int)left;
}

bool run_done(const struct run *run)
{
	return !run || run->state == ST_DONE;
}

static void one_message(struct run *run)
{
	struct run_result *r = run->r;
	char *msg = run->msg;
	ssize_t n = recv(run->sock, msg, DESC_MAX_JSON + 15, MSG_DONTWAIT);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n <= 0) { /* the child is gone */
		close(run->sock);
		run->sock = -1;
		run->state = ST_REAPING;
		run->reap_end = now_ms() + 1000;
		return;
	}
	msg[n] = 0;
	if (msg[0] == MSG_OPEN) {
		if (memchr(msg + 1, 0, (size_t)n - 1))
			send(run->sock, "Nd" "not a valid path", 18, MSG_NOSIGNAL);
		else
			reply_fd(run->sock, &run->b, msg + 1);
	} else if (msg[0] == MSG_PRINT) {
		if (r->nlog < RUN_MAX_LOG && utf8_valid(msg + 1, (size_t)n - 1))
			r->log[r->nlog++] = strdup(msg + 1);
	} else if (msg[0] == MSG_DONE && !run->got_final) {
		run->got_final = true;
		char err[256];
		struct value *v = v_from_json(msg + 1, (size_t)n - 1, 32, err, sizeof err);
		struct value *d = v ? desc_normalize(v, run->edit, broker_path_check, &run->b, err, sizeof err) : NULL;
		v_free(v);
		if (!d) {
			r->status = RUN_ERROR;
			snprintf(r->msg, sizeof r->msg, "invalid output: %s", err);
		} else {
			r->status = RUN_OK;
			r->desc = d;
			v_to_json(d, &r->json);
			r->hash = hash64(r->json.p, r->json.len);
		}
	} else if (msg[0] == MSG_ERROR && !run->got_final && n >= 2) {
		run->got_final = true;
		r->status = msg[1] == ERR_TIME ? RUN_TIME : msg[1] == ERR_MEMORY ? RUN_MEMORY : RUN_ERROR;
		size_t l = (size_t)n - 2;
		if (!utf8_valid(msg + 2, l))
			l = 0;
		snprintf(r->msg, sizeof r->msg, "%.*s", (int)l, msg + 2);
	}
}

static void finish(struct run *run)
{
	struct run_result *r = run->r;
	int status = run->status;
	free(run->msg);
	run->msg = NULL;
	if (run->killed) {
		run_result_free(r);
		r->status = RUN_TIMEOUT;
		snprintf(r->msg, sizeof r->msg, "stopped after %d ms", run->wall);
	} else if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		v_free(r->desc);
		r->desc = NULL;
		buf_free(&r->json);
		if (sig == SIGSYS) {
			r->status = RUN_BLOCKED;
			snprintf(r->msg, sizeof r->msg, "stopped: a system call widgets may not make");
		} else if (sig == SIGPROF || sig == SIGXCPU) {
			r->status = RUN_TIME;
			snprintf(r->msg, sizeof r->msg, "over the time limit");
		} else {
			r->status = RUN_CRASH;
			snprintf(r->msg, sizeof r->msg, "died of signal %d", sig);
		}
	} else if (r->status == RUN_OK && WEXITSTATUS(status) != 0) {
		v_free(r->desc);
		r->desc = NULL;
		buf_free(&r->json);
		r->status = RUN_CRASH;
		snprintf(r->msg, sizeof r->msg, "exited with status %d", WEXITSTATUS(status));
	} else if (!run->got_final) {
		r->status = RUN_CRASH;
		snprintf(r->msg, sizeof r->msg, "ended without output (status %d)", WEXITSTATUS(status));
	}
	for (size_t i = 0; i < run->b.nfiles; i++) {
		r->files[i] = run->b.files[i];
		run->b.files[i] = NULL;
	}
	r->nfiles = run->b.nfiles;
	run->b.nfiles = 0;
	broker_free(&run->b);
	run->state = ST_DONE;
}

/* The exit status of our child: kept ones first, then one message from the zygote. */
static bool reap_one(struct run *run)
{
	struct zygote *z = run->z;
	for (int i = 0; i < z->ndone; i++)
		if (z->done[i].pid == run->pid) {
			run->status = z->done[i].status;
			z->done[i] = z->done[--z->ndone];
			return true;
		}
	char type;
	pid_t pid;
	int st = 0;
	if (!zygote_recv(z, &type, &pid, &st, 0))
		return false;
	if (type != MSG_EXITED)
		return false;
	if (pid == run->pid) {
		run->status = st;
		return true;
	}
	if (z->ndone < (int)(sizeof z->done / sizeof *z->done)) {
		z->done[z->ndone].pid = pid;
		z->done[z->ndone++].status = st;
	}
	return false;
}

bool run_step(struct run *run)
{
	if (!run || run->state == ST_DONE)
		return true;
	if (run->state == ST_RUNNING) {
		if (now_ms() >= run->start + run->wall) {
			kill(run->pid, SIGKILL);
			run->killed = true;
			close(run->sock);
			run->sock = -1;
			run->state = ST_REAPING;
			run->reap_end = now_ms() + 2000;
			return false;
		}
		struct pollfd p = { run->sock, POLLIN, 0 };
		while (run->state == ST_RUNNING && poll(&p, 1, 0) > 0)
			one_message(run);
		if (run->state == ST_RUNNING)
			return false;
	}
	/* reaping */
	while (!reap_one(run)) {
		struct pollfd p = { run->z->fd, POLLIN, 0 };
		if (poll(&p, 1, 0) > 0)
			continue;
		if (now_ms() < run->reap_end)
			return false;
		if (!run->kill_sent) {
			kill(run->pid, SIGKILL);
			run->kill_sent = true;
			run->reap_end = now_ms() + 2000;
			return false;
		}
		break; /* gave up on the exit status */
	}
	finish(run);
	return true;
}

void run_free(struct run *run)
{
	if (!run)
		return;
	if (run->state != ST_DONE) {
		if (run->sock >= 0)
			close(run->sock);
		kill(run->pid, SIGKILL);
		free(run->msg);
		broker_free(&run->b);
	}
	free(run);
}

void run_widget(struct zygote *z, const char *dir, const struct manifest *m, const struct value *props,
		const struct run_opts *o, struct run_result *r)
{
	struct run *run = run_start(z, dir, m, props, o, r);
	while (!run_step(run)) {
		struct pollfd p = { run_fd(run), POLLIN, 0 };
		poll(&p, 1, run_timeout_ms(run));
	}
	run_free(run);
}
