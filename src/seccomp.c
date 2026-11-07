// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The seccomp allowlist of a widget run. Everything not listed kills the process.
 * The run may only: read from fds it was given, write and send/receive on its socket,
 * close fds, manage memory (never executable), read clocks and exit.
 */
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include "sandbox.h"

#if defined(__x86_64__)
#define ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define ARCH AUDIT_ARCH_AARCH64
#else
#error "add this architecture's AUDIT_ARCH to seccomp.c"
#endif

#define LOAD_NR BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr))
#define ALLOW BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define KILL BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)
/* if nr == n: allow, else fall through */
#define SYS_ALLOW(n) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (n), 0, 1), ALLOW
/* if nr == n: allow unless args[2] (mmap's prot) asks for PROT_EXEC, then kill; else fall through */
#define SYS_NO_EXEC(n) \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (n), 0, 4), \
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[2])), \
	BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1), \
	KILL, \
	ALLOW
/* if nr == n: allow only when args[0] == fd (both halves), else kill */
#define SYS_ON_FD(n, fd) \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (n), 0, 6), \
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])), \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)(fd), 0, 3), \
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]) + 4), \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1), \
	ALLOW, \
	KILL

bool seccomp_install(int sock)
{
	struct sock_filter f[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ARCH, 1, 0),
		KILL,
		LOAD_NR,
#ifdef __x86_64__
		/* the x32 ABI shares the arch value */
		BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
		KILL,
#endif
		SYS_ALLOW(__NR_read),
		SYS_ALLOW(__NR_close),
		/* no executable memory: mprotect isn't allowed at all, mremap keeps a mapping's protection */
		SYS_NO_EXEC(__NR_mmap),
		SYS_ALLOW(__NR_munmap),
		SYS_ALLOW(__NR_mremap),
		SYS_ALLOW(__NR_brk),
		SYS_ALLOW(__NR_madvise),
		SYS_ALLOW(__NR_futex),
		SYS_ALLOW(__NR_clock_gettime),
		SYS_ALLOW(__NR_exit),
		SYS_ALLOW(__NR_exit_group),
		SYS_ALLOW(__NR_rt_sigreturn),
		SYS_ON_FD(__NR_write, sock),
		LOAD_NR,
		SYS_ON_FD(__NR_sendmsg, sock),
		LOAD_NR,
		SYS_ON_FD(__NR_recvmsg, sock),
		KILL,
	};
	struct sock_fprog prog = { .len = sizeof f / sizeof f[0], .filter = f };
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return false;
	return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0;
}
