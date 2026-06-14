// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test for the SPSC ring: capacity rounding, planar round-trip, underrun
 * zero-fill, and overrun drop-oldest. No PipeWire / libreac needed. */

#include "../src/reac_ring.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>

#define CH 4

int main(void)
{
	struct reac_ring r;
	assert(reac_ring_init(&r, CH, 100) == 0);
	assert(r.capacity == 128);          /* rounded up to power of two */
	assert(reac_ring_readable(&r) == 0);

	/* write 12 frames/ch (a REAC quantum), planar src[c*n + s] = c*100 + s */
	float src[CH * 12];
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < 12; s++)
			src[c * 12 + s] = (float)(c * 100 + s);
	assert(reac_ring_write(&r, src, 12) == 12);
	assert(reac_ring_readable(&r) == 12);

	/* read 8 frames/ch back into planar dst, check values. dst buffers are sized
	 * 16 so the later 10-frame read can't overflow them. */
	float d0[16], d1[16], d2[16], d3[16];
	float *dst[CH] = { d0, d1, d2, d3 };
	uint32_t got = reac_ring_read_planar(&r, dst, CH, 8);
	assert(got == 8);
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < 8; s++)
			assert(fabsf(dst[c][s] - (float)(c * 100 + s)) < 1e-6f);
	assert(reac_ring_readable(&r) == 4);

	/* underrun: ask for 10 with only 4 left -> 6 zeros, underrun bumped */
	got = reac_ring_read_planar(&r, dst, CH, 10);
	assert(got == 4);
	for (int c = 0; c < CH; c++) {
		for (int s = 0; s < 4; s++)
			assert(fabsf(dst[c][s] - (float)(c * 100 + 8 + s)) < 1e-6f);
		for (int s = 4; s < 10; s++)
			assert(dst[c][s] == 0.0f);
	}
	assert(atomic_load(&r.underruns) == 6);

	/* overrun: fill the ring past capacity, oldest dropped, newest kept */
	float big[CH * 200];
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < 200; s++)
			big[c * 200 + s] = (float)s;
	/* a single write clamps to mask (127): request 200, write 127 (ring is empty
	 * here so this exercises the per-write clamp, not the overrun-drop path) */
	uint32_t w = reac_ring_write(&r, big, 200);
	assert(w == r.mask);                /* clamped to one ring's worth */
	assert(reac_ring_readable(&r) == r.mask);

	/* real overrun: ring is now full (readable == mask, writable == 0). A further
	 * write drops the NEWEST (SPSC-safe: the producer must not move tail), so it
	 * writes 0, readable is unchanged, and overruns is bumped by the dropped count. */
	uint64_t ov0 = atomic_load(&r.overruns);
	float more[CH * 4];
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < 4; s++)
			more[c * 4 + s] = -1.0f;
	uint32_t w2 = reac_ring_write(&r, more, 4);
	assert(w2 == 0);
	assert(reac_ring_readable(&r) == r.mask);
	assert(atomic_load(&r.overruns) == ov0 + 4);

	reac_ring_free(&r);
	printf("test_reac_ring: OK\n");
	return 0;
}
