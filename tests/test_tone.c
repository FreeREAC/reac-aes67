// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <stdint.h>
#include "../src/tone.h"
#include "../src/reac_mode.h"
#include "test_util.h"

static int32_t load24(const uint8_t *p)
{
	int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
	if (v & 0x800000) v |= ~0xFFFFFF;
	return v;
}

static int peak(const uint8_t *pcm, const struct reac_mode *m)
{
	int32_t mx = 0;
	for (int i = 0; i < m->n_channels * m->samples_per_pkt; i++) {
		int32_t v = load24(pcm + (size_t)i * 3), a = v < 0 ? -v : v;
		if (a > mx) mx = a;
	}
	return mx;
}

static int test_amplitude_bounded(void)
{
	struct tone t; tone_init(&t, 1000, 4000000, 0);
	uint8_t pcm[REAC_MAX_CHANNELS * 12 * 3];
	tone_fill(&t, pcm, &REAC_MODE_48K);
	int pk = peak(pcm, &REAC_MODE_48K);
	ASSERT(pk <= 4000000);     /* within requested amplitude */
	ASSERT(pk <= 8388607);     /* within 24-bit range */
	return 0;
}

static int test_not_silent(void)
{
	struct tone t; tone_init(&t, 1000, 4000000, 0);
	uint8_t pcm[REAC_MAX_CHANNELS * 12 * 3];
	/* advance a few packets so we're off the zero crossing */
	for (int k = 0; k < 3; k++) tone_fill(&t, pcm, &REAC_MODE_48K);
	ASSERT(peak(pcm, &REAC_MODE_48K) > 100000); /* clearly nonzero signal */
	return 0;
}

static int test_phase_continuous_across_packets(void)
{
	/* last sample of packet N and first of packet N+1 should be close
	 * (continuous sine, no jump) — within one sample-step of the waveform. */
	struct tone t; tone_init(&t, 1000, 4000000, 0);
	uint8_t a[REAC_MAX_CHANNELS * 12 * 3], b[REAC_MAX_CHANNELS * 12 * 3];
	tone_fill(&t, a, &REAC_MODE_48K);
	tone_fill(&t, b, &REAC_MODE_48K);
	int ns = REAC_MODE_48K.samples_per_pkt;
	int32_t last = load24(a + (size_t)(0 * ns + (ns - 1)) * 3); /* ch0 last */
	int32_t first = load24(b + (size_t)(0 * ns + 0) * 3);       /* ch0 first */
	int32_t step = first - last; if (step < 0) step = -step;
	/* one step of a 1kHz sine at 48k * amplitude ~ 2*pi*1000/48000*4e6 ~ 524k */
	ASSERT(step < 700000);
	return 0;
}

static int test_per_channel_detune_differs(void)
{
	struct tone t; tone_init(&t, 1000, 4000000, 50); /* +50Hz per channel */
	uint8_t pcm[REAC_MAX_CHANNELS * 12 * 3];
	for (int k = 0; k < 5; k++) tone_fill(&t, pcm, &REAC_MODE_48K);
	int ns = REAC_MODE_48K.samples_per_pkt;
	/* ch0 and ch39 are different frequencies -> their sample-0 should differ */
	int32_t c0 = load24(pcm + (size_t)(0 * ns) * 3);
	int32_t c39 = load24(pcm + (size_t)(39 * ns) * 3);
	ASSERT(c0 != c39);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_amplitude_bounded);
	rc |= RUN(test_not_silent);
	rc |= RUN(test_phase_continuous_across_packets);
	rc |= RUN(test_per_channel_detune_differs);
	return rc;
}
