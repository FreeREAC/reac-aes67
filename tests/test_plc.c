// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <string.h>
#include <stdint.h>
#include "../src/plc.h"
#include "../src/reac_mode.h"
#include "test_util.h"

static void store24(uint8_t *p, int32_t x)
{
	p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); p[2] = (uint8_t)(x >> 16);
}
static int32_t load24(const uint8_t *p)
{
	int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
	if (v & 0x800000) v |= ~0xFFFFFF;
	return v;
}

/* fill a planar packet: channel ch, sample s = gen(ch, s) */
typedef int32_t (*gen_fn)(int ch, int s);
static void fill(uint8_t *pcm, const struct reac_mode *m, gen_fn g)
{
	for (int ch = 0; ch < m->n_channels; ch++)
		for (int s = 0; s < m->samples_per_pkt; s++)
			store24(pcm + (size_t)(ch * m->samples_per_pkt + s) * 3, g(ch, s));
}

static int32_t gen_zero(int ch, int s) { (void)ch; (void)s; return 0; }
static int32_t gen_sine(int ch, int s) { (void)ch; /* ~1kHz-ish ramp-sine proxy */
	static const int32_t tbl[12] = {0, 200000, 360000, 420000, 360000, 200000,
	                                 0, -200000, -360000, -420000, -360000, -200000};
	return tbl[s % 12];
}
static uint32_t rng = 12345;
static int32_t gen_noise(int ch, int s) { (void)ch; (void)s;
	rng = rng * 1103515245u + 12345u;
	return (int32_t)((rng >> 8) & 0x7fffff) - 0x400000; /* +-4M, in 24-bit range */
}
static int32_t gen_fullscale(int ch, int s) { (void)ch; (void)s; return 8388607; }

static int peak_abs(const uint8_t *pcm, const struct reac_mode *m)
{
	int32_t mx = 0;
	int total = m->n_channels * m->samples_per_pkt;
	for (int i = 0; i < total; i++) {
		int32_t v = load24(pcm + (size_t)i * 3);
		int32_t a = v < 0 ? -v : v;
		if (a > mx) mx = a;
	}
	return mx;
}

static int test_silence_stays_silent(void)
{
	struct plc st;
	plc_init(&st, &REAC_MODE_48K, 0, 0);
	uint8_t pcm[REAC_MAX_CHANNELS * 12 * 3];
	fill(pcm, &REAC_MODE_48K, gen_zero);
	plc_observe(&st, pcm);
	uint8_t out[REAC_MAX_CHANNELS * 12 * 3];
	int tier = plc_conceal(&st, out, 0);
	ASSERT_EQ_INT(tier, PLC_TIER_REPEAT_XFADE);
	ASSERT_EQ_INT(peak_abs(out, &REAC_MODE_48K), 0); /* silence in -> silence out */
	return 0;
}

static int test_noise_bounded_no_overflow(void)
{
	struct plc st;
	plc_init(&st, &REAC_MODE_48K, 0, 0);
	uint8_t pcm[REAC_MAX_CHANNELS * 12 * 3];
	fill(pcm, &REAC_MODE_48K, gen_noise);
	int in_peak = peak_abs(pcm, &REAC_MODE_48K);
	plc_observe(&st, pcm);
	uint8_t out[REAC_MAX_CHANNELS * 12 * 3];
	plc_conceal(&st, out, 0);
	int out_peak = peak_abs(out, &REAC_MODE_48K);
	ASSERT(out_peak <= 8388607);          /* in 24-bit range, no wrap */
	ASSERT(out_peak <= in_peak);          /* repeat/crossfade never amplifies */
	return 0;
}

static int test_sine_no_value_step_at_splice(void)
{
	struct plc st;
	plc_init(&st, &REAC_MODE_48K, 0, 0);
	uint8_t pcm[REAC_MAX_CHANNELS * 12 * 3];
	fill(pcm, &REAC_MODE_48K, gen_sine);
	plc_observe(&st, pcm);
	uint8_t out[REAC_MAX_CHANNELS * 12 * 3];
	plc_conceal(&st, out, 0);
	/* last good sample of ch0 = tbl[11] = -200000; first concealed should be
	 * close (crossfade from real tail), not a big jump. max adjacent delta of
	 * the source table is 200000; assert the splice step is within ~2x that. */
	int32_t last_good = -200000;
	int32_t first_conceal = load24(out + 0);
	int32_t step = first_conceal - last_good;
	if (step < 0) step = -step;
	ASSERT(step <= 2 * 200000);
	return 0;
}

static int test_burst_monotone_decay_to_silence(void)
{
	struct plc st;
	/* small fade window so decay is visible in a few steps; flat=0 */
	plc_init(&st, &REAC_MODE_48K, 4, 0);
	st.fade_flat_packets = 0;
	uint8_t pcm[REAC_MAX_CHANNELS * 12 * 3];
	fill(pcm, &REAC_MODE_48K, gen_fullscale);
	plc_observe(&st, pcm);
	uint8_t out[REAC_MAX_CHANNELS * 12 * 3];
	int prev = 1 << 30;
	for (int k = 0; k <= 4; k++) {
		plc_conceal(&st, out, k);
		int pk = peak_abs(out, &REAC_MODE_48K);
		ASSERT(pk <= prev);  /* monotone non-increasing */
		prev = pk;
	}
	/* by k == fade_packets, fully silent */
	plc_conceal(&st, out, 4);
	ASSERT_EQ_INT(peak_abs(out, &REAC_MODE_48K), 0);
	return 0;
}

static int test_cold_start_silence_then_hold(void)
{
	struct plc st;
	plc_init(&st, &REAC_MODE_48K, 0, 0);
	uint8_t out[REAC_MAX_CHANNELS * 12 * 3];
	/* no observe yet -> cold start -> silence */
	int tier = plc_conceal(&st, out, 0);
	ASSERT_EQ_INT(tier, PLC_TIER_SILENCE);
	ASSERT_EQ_INT(peak_abs(out, &REAC_MODE_48K), 0);
	return 0;
}

static int test_96k_mode_shape(void)
{
	struct plc st;
	plc_init(&st, &REAC_MODE_96K, 0, 0);
	ASSERT_EQ_INT(st.S, 12);
	ASSERT_EQ_INT(st.n_channels, 40);
	uint8_t pcm[REAC_MAX_CHANNELS * 12 * 3];
	fill(pcm, &REAC_MODE_96K, gen_sine);
	plc_observe(&st, pcm);
	uint8_t out[REAC_MAX_CHANNELS * 12 * 3];
	int tier = plc_conceal(&st, out, 0);
	ASSERT_EQ_INT(tier, PLC_TIER_REPEAT_XFADE);
	return 0;
}

/* channel 0 sine, channel 1 silent -> concealed ch1 stays silent (no bleed) */
static int32_t gen_ch0sine_ch1zero(int ch, int s)
{
	return ch == 0 ? gen_sine(0, s) : 0;
}
static int test_per_channel_independence(void)
{
	struct plc st;
	plc_init(&st, &REAC_MODE_48K, 0, 0);
	uint8_t pcm[REAC_MAX_CHANNELS * 12 * 3];
	fill(pcm, &REAC_MODE_48K, gen_ch0sine_ch1zero);
	plc_observe(&st, pcm);
	uint8_t out[REAC_MAX_CHANNELS * 12 * 3];
	plc_conceal(&st, out, 0);
	/* channel 1's 12 samples must all be 0 */
	for (int s = 0; s < 12; s++) {
		int32_t v = load24(out + (size_t)(1 * 12 + s) * 3);
		ASSERT_EQ_INT(v, 0);
	}
	/* channel 0 must NOT be all zero */
	int ch0nz = 0;
	for (int s = 0; s < 12; s++)
		if (load24(out + (size_t)(0 * 12 + s) * 3)) ch0nz = 1;
	ASSERT(ch0nz);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_silence_stays_silent);
	rc |= RUN(test_noise_bounded_no_overflow);
	rc |= RUN(test_sine_no_value_step_at_splice);
	rc |= RUN(test_burst_monotone_decay_to_silence);
	rc |= RUN(test_cold_start_silence_then_hold);
	rc |= RUN(test_96k_mode_shape);
	rc |= RUN(test_per_channel_independence);
	return rc;
}
