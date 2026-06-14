// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* plc.c — integer, per-channel, search-free packet loss concealment.
 * See plc.h for the strategy + fallback ladder. ~150 lines. */
#include "plc.h"
#include <string.h>

#define SMAX 8388607
#define SMIN (-8388608)
#define Q15 32768

static inline int32_t load24(const uint8_t *p)
{
	int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
	if (v & 0x800000) v |= ~0xFFFFFF;
	return v;
}
static inline void store24(uint8_t *p, int32_t x)
{
	if (x > SMAX) x = SMAX; else if (x < SMIN) x = SMIN;
	p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); p[2] = (uint8_t)(x >> 16);
}

/* back-th newest sample (0 = newest), 0 if not present */
static inline int32_t hist_at(const struct plc_chan *c, int back)
{
	if (back >= c->filled) return 0;
	int idx = (c->head - 1 - back) % PLC_HIST_SAMPLES;
	if (idx < 0) idx += PLC_HIST_SAMPLES;
	return c->hist[idx];
}
static inline void hist_push(struct plc_chan *c, int32_t v)
{
	c->hist[c->head] = v;
	c->head = (c->head + 1) % PLC_HIST_SAMPLES;
	if (c->filled < PLC_HIST_SAMPLES) c->filled++;
}

void plc_init(struct plc *st, const struct reac_mode *mode,
              int fade_packets, int xfade_w)
{
	memset(st, 0, sizeof *st);
	st->S = mode->samples_per_pkt;
	st->n_channels = mode->n_channels;
	st->xfade_w = xfade_w > 0 ? xfade_w : (st->S < 8 ? st->S : 8);
	if (st->xfade_w > st->S) st->xfade_w = st->S;
	st->fade_packets = fade_packets > 0 ? fade_packets : 200;
	st->fade_flat_packets = st->fade_packets / 5;
}

void plc_observe(struct plc *st, const uint8_t *pcm)
{
	int S = st->S;
	for (int ch = 0; ch < st->n_channels; ch++) {
		const uint8_t *base = pcm + (size_t)ch * S * 3;
		for (int s = 0; s < S; s++)
			hist_push(&st->ch[ch], load24(base + (size_t)s * 3));
	}
}

/* Q15 burst gain for loss_index k: flat for fade_flat_packets, then linear to 0. */
static int32_t burst_gain_q15(const struct plc *st, int k)
{
	if (k <= st->fade_flat_packets) return Q15;
	if (k >= st->fade_packets) return 0;
	int span = st->fade_packets - st->fade_flat_packets;
	int into = k - st->fade_flat_packets;
	return (int32_t)Q15 * (span - into) / span;
}

int plc_conceal(struct plc *st, uint8_t *out, int loss_index)
{
	int S = st->S, W = st->xfade_w, tier = PLC_TIER_REPEAT_XFADE;
	int32_t g = burst_gain_q15(st, loss_index);

	for (int ch = 0; ch < st->n_channels; ch++) {
		struct plc_chan *c = &st->ch[ch];
		uint8_t *o = out + (size_t)ch * S * 3;

		if (c->filled == 0) {                 /* TIER 3: cold start */
			for (int s = 0; s < S; s++) store24(o + (size_t)s * 3, 0);
			if (tier < PLC_TIER_SILENCE) tier = PLC_TIER_SILENCE;
			continue;
		}
		if (c->filled < S) {                  /* TIER 2: hold-last (DC) */
			int32_t hv = (int32_t)(((int64_t)hist_at(c, 0) * g) >> 15);
			for (int s = 0; s < S; s++) store24(o + (size_t)s * 3, hv);
			if (tier < PLC_TIER_HOLD_LAST) tier = PLC_TIER_HOLD_LAST;
			continue;
		}

		if (loss_index == 0) {                /* TIER 0: repeat + linear xfade */
			/* Anchor the seam on the last real sample (hold value) so out[0]
			 * starts at x_{-1} with NO value-step, then ramp into the verbatim
			 * repeat of the previous packet. (A blend against a phase-shifted
			 * "real tail" would mis-anchor — see test_sine_no_value_step.) */
			int32_t last = hist_at(c, 0);
			for (int s = 0; s < S; s++) {
				int32_t sub = hist_at(c, S - 1 - s); /* previous packet, sample s */
				int32_t y;
				if (s < W) {
					int32_t a = (int32_t)((s + 1) * Q15) / (W + 1); /* 0->1 */
					y = (int32_t)(((int64_t)last * (Q15 - a) + (int64_t)sub * a) >> 15);
				} else {
					y = sub;
				}
				store24(o + (size_t)s * 3, y);
			}
		} else {                              /* TIER 1: scaled-repeat (burst) */
			if (tier < PLC_TIER_BURST_FADE) tier = PLC_TIER_BURST_FADE;
			for (int s = 0; s < S; s++) {
				int32_t sub = hist_at(c, S - 1 - s);
				int32_t y = (int32_t)(((int64_t)sub * g) >> 15);
				store24(o + (size_t)s * 3, y);
			}
		}
	}
	return tier;
}
