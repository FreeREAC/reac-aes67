// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Tiny dependency-free test harness. */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

#define ASSERT(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "  ASSERT failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
		return 1; \
	} \
} while (0)

#define ASSERT_EQ_INT(a, b) do { \
	long _a = (long)(a), _b = (long)(b); \
	if (_a != _b) { \
		fprintf(stderr, "  ASSERT_EQ failed: %s=%ld != %s=%ld (%s:%d)\n", \
		        #a, _a, #b, _b, __FILE__, __LINE__); \
		return 1; \
	} \
} while (0)

#define RUN(fn) run_test(#fn, fn)

static inline int run_test(const char *name, int (*fn)(void))
{
	int r = fn();
	fprintf(stderr, "%s %s\n", r ? "FAIL" : "ok  ", name);
	return r;
}

#endif /* TEST_UTIL_H */
