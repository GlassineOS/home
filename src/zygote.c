// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sandbox.h"

#define ZBUF (RUN_MAX_SCRIPT + 64 * 1024)

static uint64_t address_space_limit(void)
{
	/* what the zygote maps now, plus room for a run's heap and its data files */
	unsigned long pages = 0;
	FILE *f = fopen("/proc/self/statm", "re");
	if (f) {
		if (fscanf(f, "%lu", &pages) != 1)
			pages = 0;
		fclose(f);
	}
	uint64_t vsz = (uint64_t)pages * (uint64_t)sysconf(_SC_PAGESIZE);
	return vsz + 256ull * 1024 * 1024;
}

static void send_exit(int fd, pid_t pid, int status)
{
	char m[1 + sizeof pid + sizeof status];
	m[0] = MSG_EXITED;
	memcpy(m + 1, &pid, sizeof pid);
	memcpy(m + 1 + sizeof pid, &status, sizeof status);
	send(fd, m, sizeof m, MSG_NOSIGNAL);
}

static void close_others(int keep)
{
	if (keep != 3) {
		dup2(keep, 3);
		close(keep);
	}
	if (syscall(SYS_close_range, 4u, ~0u, 0u) != 0)
		for (int i = 4; i < 4096; i++)
			close(i);
	for (int i = 0; i < 3; i++)
		close(i);
}

static void zygote_main(int fd) __attribute__((noreturn));
static void zygote_main(int fd)
{
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	prctl(PR_SET_NAME, "home-zygote");
	if (getppid() == 1)
		_exit(0);
	uint64_t as = address_space_limit();
	sigset_t chld;
	sigemptyset(&chld);
	sigaddset(&chld, SIGCHLD);
	sigprocmask(SIG_BLOCK, &chld, NULL);
	int sfd = signalfd(-1, &chld, SFD_CLOEXEC | SFD_NONBLOCK);
	char *buf = malloc(ZBUF);
	if (sfd < 0 || !buf)
		_exit(1);
	for (;;) {
		struct pollfd p[2] = { { fd, POLLIN, 0 }, { sfd, POLLIN, 0 } };
		if (poll(p, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			_exit(1);
		}
		if (p[1].revents) {
			struct signalfd_siginfo si;
			while (read(sfd, &si, sizeof si) == sizeof si)
				;
			int status;
			pid_t pid;
			while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
				send_exit(fd, pid, status);
		}
		if (!p[0].revents)
			continue;
		char cbuf[CMSG_SPACE(sizeof(int))];
		struct iovec iov = { buf, ZBUF };
		struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
		ssize_t n = recvmsg(fd, &mh, MSG_CMSG_CLOEXEC);
		if (n <= 0)
			_exit(0); /* Home is gone */
		int run_fd = -1;
		for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
			if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
				memcpy(&run_fd, CMSG_DATA(c), sizeof run_fd);
		pid_t pid = -1;
		if (run_fd >= 0 && (size_t)n >= sizeof(struct run_req) && !(mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
			pid = fork();
			if (pid == 0) {
				sigset_t none;
				sigemptyset(&none);
				signal(SIGCHLD, SIG_DFL);
				sigprocmask(SIG_SETMASK, &none, NULL);
				prctl(PR_SET_PDEATHSIG, SIGKILL);
				prctl(PR_SET_NAME, "home-widget");
				close(sfd);
				close(fd);
				close_others(run_fd);
				child_main(3, (const struct run_req *)buf, (size_t)n, as);
			}
		}
		if (run_fd >= 0)
			close(run_fd);
		char m[1 + sizeof pid];
		m[0] = MSG_SPAWNED;
		memcpy(m + 1, &pid, sizeof pid);
		send(fd, m, sizeof m, MSG_NOSIGNAL);
	}
}

bool zygote_start(struct zygote *z)
{
	int sv[2];
	memset(z, 0, sizeof *z);
	z->fd = -1;
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv))
		return false;
	int sz = ZBUF + 4096;
	setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
	setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
	pid_t pid = fork();
	if (pid < 0) {
		close(sv[0]);
		close(sv[1]);
		return false;
	}
	if (pid == 0) {
		close(sv[0]);
		zygote_main(sv[1]);
	}
	close(sv[1]);
	z->pid = pid;
	z->fd = sv[0];
	return true;
}

void zygote_stop(struct zygote *z)
{
	if (z->fd >= 0)
		close(z->fd);
	if (z->pid > 0)
		waitpid(z->pid, NULL, 0);
	z->fd = -1;
	z->pid = 0;
}
