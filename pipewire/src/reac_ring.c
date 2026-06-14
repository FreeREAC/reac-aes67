// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_ring.h"
#include <stdlib.h>
#include <string.h>

static uint32_t next_pow2(uint32_t v)
{
	if (v < 2)
		return 2;
	v--;
	v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
	return v + 1;
}

int reac_ring_init(struct reac_ring *r, uint32_t channels, uint32_t capacity_frames)
{
	uint32_t cap = next_pow2(capacity_frames);
	r->buf = calloc((size_t)cap * channels, sizeof(float));
	if (!r->buf)
		return -1;
	r->capacity = cap;
	r->mask = cap - 1;
	r->channels = channels;
	atomic_store_explicit(&r->head, 0, memory_order_relaxed);
	atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
	atomic_store_explicit(&r->underruns, 0, memory_order_relaxed);
	atomic_store_explicit(&r->overruns, 0, memory_order_relaxed);
	return 0;
}

void reac_ring_free(struct reac_ring *r)
{
	free(r->buf);
	r->buf = NULL;
}

uint32_t reac_ring_readable(const struct reac_ring *r)
{
	uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
	uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
	return (h - t) & r->mask;
}

uint32_t reac_ring_writable(const struct reac_ring *r)
{
	/* leave one slot empty to disambiguate full from empty */
	return r->mask - reac_ring_readable(r);
}

uint32_t reac_ring_write(struct reac_ring *r, const float *planar, uint32_t n)
{
	if (n > r->mask)
		n = r->mask; /* a single frame can't exceed the ring */
	const uint32_t stride = n; /* per-channel stride in `planar` (never shrinks) */

	uint32_t free_slots = reac_ring_writable(r);
	uint32_t w = n;
	if (w > free_slots) {
		/* Overrun (transient): the ring is full. SPSC discipline forbids the
		 * PRODUCER from moving `tail` (only the consumer may) — so drop the
		 * NEWEST samples that don't fit here rather than racing the consumer to
		 * drop the oldest. Overrun should be rare: the resampler holds the fill
		 * near target, so this is only a safety net. (Bounded-latency trimming of
		 * the OLDEST, if wanted, belongs on the consumer side where moving `tail`
		 * is legal.) */
		atomic_fetch_add_explicit(&r->overruns, w - free_slots, memory_order_relaxed);
		w = free_slots;
		if (w == 0)
			return 0;
	}

	uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
	for (uint32_t c = 0; c < r->channels; c++) {
		float *slot = r->buf + (size_t)c * r->capacity;
		const float *src = planar + (size_t)c * stride;
		uint32_t pos = h;
		for (uint32_t s = 0; s < w; s++) {
			slot[pos] = src[s];
			pos = (pos + 1) & r->mask;
		}
	}
	atomic_store_explicit(&r->head, (h + w) & r->mask, memory_order_release);
	return w;
}

uint32_t reac_ring_read_planar(struct reac_ring *r, float *const *dst,
                               uint32_t channels, uint32_t n)
{
	uint32_t avail = reac_ring_readable(r);
	uint32_t real = avail < n ? avail : n;
	uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	uint32_t nch = channels < r->channels ? channels : r->channels;

	for (uint32_t c = 0; c < nch; c++) {
		const float *slot = r->buf + (size_t)c * r->capacity;
		float *out = dst[c];
		uint32_t pos = t;
		for (uint32_t s = 0; s < real; s++) {
			out[s] = slot[pos];
			pos = (pos + 1) & r->mask;
		}
		/* underrun: zero-fill the remainder so the DAC never gets garbage */
		for (uint32_t s = real; s < n; s++)
			out[s] = 0.0f;
	}
	atomic_store_explicit(&r->tail, (t + real) & r->mask, memory_order_release);
	if (real < n)
		atomic_fetch_add_explicit(&r->underruns, n - real, memory_order_relaxed);
	return real;
}

uint32_t reac_ring_trim(struct reac_ring *r, uint32_t keep)
{
	uint32_t avail = reac_ring_readable(r);
	if (avail <= keep)
		return 0;
	uint32_t drop = avail - keep;
	uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	atomic_store_explicit(&r->tail, (t + drop) & r->mask, memory_order_release);
	return drop;
}
