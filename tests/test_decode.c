// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Tests for reac_decode against the real pcap fixture shared with reac-tools.
 *
 * Ground truth (extracted from reac-tools/tests/fixtures/real_reac_stream.pcap,
 * frame 0): seq=0xfd7e, end marker 0xC2 0xEA, and decoded 48k sample
 * channel 4 / time-sample 0 == 0xFF 0xFF 0xFF (24-bit -1). Other early samples
 * are digital silence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/reac_decode.h"
#include "../src/reac_mode.h"
#include "test_util.h"

/* Loads the first REAC frame (full ethernet bytes) from the shared fixture.
 * Returns malloc'd buffer + length, or NULL. */
static uint8_t *load_first_frame(size_t *len_out);

static int test_inspect_valid_48k(void)
{
	size_t len;
	uint8_t *raw = load_first_frame(&len);
	ASSERT(raw != NULL);
	ASSERT_EQ_INT((int)len, REAC_FRAME_BYTES);
	struct reac_frame f = reac_frame_inspect(raw, len, &REAC_MODE_48K);
	ASSERT(f.valid);
	ASSERT_EQ_INT(f.counter, 0xfd7e);
	free(raw);
	return 0;
}

static int test_decode_known_sample(void)
{
	size_t len;
	uint8_t *raw = load_first_frame(&len);
	ASSERT(raw != NULL);
	uint8_t out[REAC_MAX_CHANNELS * 24 * 3];
	int n = reac_decode(raw, len, &REAC_MODE_48K, out);
	ASSERT_EQ_INT(n, 12); /* samples per channel at 48k */

	/* channel 4, sample 0 -> planar index (4*12 + 0)*3 */
	size_t idx = (size_t)(4 * 12 + 0) * 3;
	ASSERT_EQ_INT(out[idx + 0], 0xff);
	ASSERT_EQ_INT(out[idx + 1], 0xff);
	ASSERT_EQ_INT(out[idx + 2], 0xff);

	/* channel 0, sample 0 -> silence */
	ASSERT_EQ_INT(out[0], 0x00);
	ASSERT_EQ_INT(out[1], 0x00);
	ASSERT_EQ_INT(out[2], 0x00);
	free(raw);
	return 0;
}

static int test_inspect_rejects_bad_endmarker(void)
{
	size_t len;
	uint8_t *raw = load_first_frame(&len);
	ASSERT(raw != NULL);
	raw[len - 1] = 0x00; /* corrupt end marker */
	struct reac_frame f = reac_frame_inspect(raw, len, &REAC_MODE_48K);
	ASSERT(!f.valid);
	free(raw);
	return 0;
}

static int test_mode_constants(void)
{
	ASSERT_EQ_INT(REAC_MODE_48K.sample_rate, 48000);
	ASSERT_EQ_INT(REAC_MODE_48K.n_channels, 40);
	ASSERT_EQ_INT(REAC_MODE_48K.samples_per_pkt, 12);
	ASSERT_EQ_INT(REAC_MODE_96K.sample_rate, 96000);
	ASSERT_EQ_INT(REAC_MODE_96K.n_channels, 40);
	ASSERT_EQ_INT(REAC_MODE_96K.samples_per_pkt, 12);
	/* both partition the same 1440 B audio region */
	ASSERT_EQ_INT(REAC_MODE_48K.n_channels * REAC_MODE_48K.samples_per_pkt * REAC_RESOLUTION, REAC_AUDIO_BYTES);
	ASSERT_EQ_INT(REAC_MODE_96K.n_channels * REAC_MODE_96K.samples_per_pkt * REAC_RESOLUTION, REAC_AUDIO_BYTES);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_mode_constants);
	rc |= RUN(test_inspect_valid_48k);
	rc |= RUN(test_decode_known_sample);
	rc |= RUN(test_inspect_rejects_bad_endmarker);
	return rc;
}

/* --- fixture loader (classic pcap, first packet) --- */
static uint8_t *load_first_frame(size_t *len_out)
{
	const char *path = getenv("REAC_FIXTURE");
	if (!path)
		path = "../reac-tools/tests/fixtures/real_reac_stream.pcap";
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "cannot open fixture %s\n", path);
		return NULL;
	}
	uint8_t gh[24];
	if (fread(gh, 1, 24, f) != 24) { fclose(f); return NULL; }
	uint8_t ph[16];
	if (fread(ph, 1, 16, f) != 16) { fclose(f); return NULL; }
	uint32_t incl = (uint32_t)ph[8] | ((uint32_t)ph[9] << 8) |
	                ((uint32_t)ph[10] << 16) | ((uint32_t)ph[11] << 24);
	uint8_t *raw = malloc(incl);
	if (!raw) { fclose(f); return NULL; }
	if (fread(raw, 1, incl, f) != incl) { free(raw); fclose(f); return NULL; }
	fclose(f);
	*len_out = incl;
	return raw;
}
