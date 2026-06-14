// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Round-trip proof: the TX encoder is the exact inverse of the reac-aes67 decode
 * core. Encode a known 40-channel block (distinct DC per channel + a sine on
 * ch0), decode it back, and assert (a) the header is well-formed, (b) every
 * sample survives within one 24-bit ULP, and (c) no channel is cross-wired —
 * each plane reads back its OWN value, which catches a wrong (s*40+ch) stride. */
#include "reac_tx.h"
#include "reac_decode.h"
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static float s24le_to_f32(const uint8_t *p)
{
	int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
	if (v & 0x00800000)
		v |= ~0x00FFFFFF;
	return (float)v / 8388608.0f;
}

#define CHK(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } } while (0)

int main(void)
{
	const struct reac_mode m = { 48000, 40, 12 };
	static const uint8_t src[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 };

	/* Build the planar input. ch0 = a half-scale sine over the 12 samples; every
	 * other channel = a distinct DC level so a stride bug shows as cross-talk. */
	float chbuf[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *planar[REAC_MAX_CHANNELS];
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
		planar[ch] = chbuf[ch];
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			chbuf[ch][s] = (ch == 0)
				? 0.5f * sinf(2.0f * (float)M_PI * (float)s / 12.0f)
				: (float)ch / 64.0f - 0.3f;
	}

	uint8_t frame[REAC_FRAME_BYTES];
	int len = reac_tx_build(frame, planar, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT,
	                        0x1234, src);
	CHK(len == REAC_FRAME_BYTES);

	/* header */
	CHK(frame[12] == 0x88 && frame[13] == 0x19);                       /* EtherType */
	CHK(frame[14] == 0x34 && frame[15] == 0x12);                       /* counter LE */
	CHK(memcmp(frame + 6, src, 6) == 0);                               /* src MAC */
	CHK(frame[REAC_FRAME_BYTES - 2] == REAC_END_MARKER_0);             /* 0xC2 */
	CHK(frame[REAC_FRAME_BYTES - 1] == REAC_END_MARKER_1);             /* 0xEA */

	/* decode + compare */
	uint8_t s24[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns = reac_decode(frame, REAC_FRAME_BYTES, &m, s24);
	CHK(ns == REAC_SAMPLES_PER_PKT);

	float maxerr = 0.0f;
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
		for (int s = 0; s < ns; s++) {
			float got = s24le_to_f32(&s24[(size_t)(ch * ns + s) * 3]);
			float want = chbuf[ch][s];
			float e = fabsf(got - want);
			if (e > maxerr)
				maxerr = e;
		}
	}
	printf("40-ch encode->decode round-trip: max abs error = %.2e (tol 1e-6)\n", maxerr);
	CHK(maxerr < 1e-6f);

	printf("OK: encoder is the inverse of the decoder; no channel cross-wiring\n");
	return 0;
}
