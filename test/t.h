/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A minimal test harness: CHECK records a failure and goes on; t_done prints a summary line. */
#ifndef GH_T_H
#define GH_T_H

#include <stdio.h>
#include <string.h>

static int t_run, t_fail;

#define CHECK(cond) do { t_run++; if (!(cond)) { t_fail++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); t_run++; \
	if (!_a || !_b || strcmp(_a, _b)) { t_fail++; fprintf(stderr, "%s:%d: got  [%s]\n%*swant [%s]\n", \
	__FILE__, __LINE__, _a ? _a : "(null)", (int)strlen(__FILE__) + 6, "", _b ? _b : "(null)"); } } while (0)
#define CHECK_INT(a, b) do { long long _a = (long long)(a), _b = (long long)(b); t_run++; \
	if (_a != _b) { t_fail++; fprintf(stderr, "%s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static inline int t_done(const char *name)
{
	printf("%-10s %d checks, %d failed\n", name, t_run, t_fail);
	return t_fail ? 1 : 0;
}

#endif
