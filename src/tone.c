// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "tone.h"
#include <math.h>
#include <stddef.h>

#define SMAX 8388607
#define SMIN (-8388608)
#define TWO_PI 6.283185307179586

static void store24(uint8_t *p, int32_t x)
{
	if (x > SMAX) x = SMAX; else if (x < SMIN) x = SMIN;
	p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); p[2] = (uint8_t)(x >> 16);
}

void tone_init(struct tone *t, int freq_hz, int amplitude, int per_channel_detune)
{
	t->phase = 0.0;
	t->freq_hz = freq_hz;
	t->amplitude = amplitude > SMAX ? SMAX : amplitude;
	t->per_channel_detune = per_channel_detune;
}

void tone_fill(struct tone *t, uint8_t *out, const struct reac_mode *mode)
{
	const int nch = mode->n_channels;
	const int ns = mode->samples_per_pkt;
	const double sr = (double)mode->sample_rate;

	/* Channel ch advances at its own frequency; phase carried per-base so the
	 * waveform is continuous across packets. We use the base phase for ch0 and
	 * add a per-channel frequency offset by scaling the sample index. */
	for (int ch = 0; ch < nch; ch++) {
		double f = (double)(t->freq_hz + ch * t->per_channel_detune);
		double w = TWO_PI * f / sr;
		for (int s = 0; s < ns; s++) {
			double v = sin(t->phase * (f / (double)t->freq_hz) + w * s);
			int32_t x = (int32_t)(v * (double)t->amplitude);
			store24(out + (size_t)(ch * ns + s) * 3, x);
		}
	}
	/* advance the base phase by ns samples at the base frequency */
	t->phase += TWO_PI * (double)t->freq_hz / sr * (double)ns;
	/* keep phase bounded */
	while (t->phase > TWO_PI) t->phase -= TWO_PI;
}
