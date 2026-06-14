// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#define _GNU_SOURCE  /* CLOCK_TAI on glibc (musl exposes it unconditionally) */
#include "ptp_clock.h"
#include <time.h>

uint32_t tai_to_rtp_ts(uint64_t tai_sec, uint32_t tai_nsec, int rate)
{
	/* sub-second sample count, rounded to nearest: round(nsec*rate/1e9).
	 * nsec*rate <= 1e9 * 192000 fits comfortably in uint64. */
	uint64_t sub = ((uint64_t)tai_nsec * (uint64_t)rate + 500000000ULL)
	             / 1000000000ULL;
	uint64_t total = tai_sec * (uint64_t)rate + sub;
	return (uint32_t)(total & 0xffffffffULL); /* mod 2^32 */
}

int ptp_now_tai(uint64_t *sec, uint32_t *nsec)
{
#ifdef CLOCK_TAI
	struct timespec ts;
	if (clock_gettime(CLOCK_TAI, &ts) != 0)
		return -1;
	if (sec)
		*sec = (uint64_t)ts.tv_sec;
	if (nsec)
		*nsec = (uint32_t)ts.tv_nsec;
	return 0;
#else
	(void)sec;
	(void)nsec;
	return -1;
#endif
}
