// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <string.h>
#include "../src/rtp_l24.h"
#include <reac/reac.h>
#include "test_util.h"

/* A tiny 2-channel, 2-sample mode for hand-checkable interleave/endianness. */
static const struct reac_mode TINY = { 48000, 2, 2 };

static int test_header_fields(void)
{
	uint8_t planar[2 * 2 * 3] = {0};
	struct rtp_params p = { .payload_type = 97, .seq = 0x1234,
	                        .timestamp = 0xDEADBEEF, .ssrc = 0x01020304 };
	uint8_t out[256];
	long n = rtp_l24_build(out, sizeof out, &p, planar, &TINY);
	ASSERT_EQ_INT(n, RTP_HEADER_LEN + 2 * 2 * 3); /* 12 + 12 */

	ASSERT_EQ_INT(out[0], (RTP_VERSION << 6));   /* V=2, no padding/ext/cc */
	ASSERT_EQ_INT(out[1], 97);                   /* M=0, PT=97 */
	ASSERT_EQ_INT(out[2], 0x12);                 /* seq hi (big-endian) */
	ASSERT_EQ_INT(out[3], 0x34);                 /* seq lo */
	ASSERT_EQ_INT(out[4], 0xDE);                 /* ts (big-endian) */
	ASSERT_EQ_INT(out[5], 0xAD);
	ASSERT_EQ_INT(out[6], 0xBE);
	ASSERT_EQ_INT(out[7], 0xEF);
	ASSERT_EQ_INT(out[8], 0x01);                 /* ssrc (big-endian) */
	ASSERT_EQ_INT(out[9], 0x02);
	ASSERT_EQ_INT(out[10], 0x03);
	ASSERT_EQ_INT(out[11], 0x04);
	return 0;
}

static int test_planar_le_to_interleaved_be(void)
{
	/* planar (channel-major), little-endian 24-bit:
	 *   ch0: s0=0x112233 -> bytes 33 22 11 ; s1=0x445566 -> 66 55 44
	 *   ch1: s0=0xAABBCC -> bytes CC BB AA ; s1=0xDDEEFF -> FF EE DD
	 */
	uint8_t planar[2 * 2 * 3] = {
		0x33,0x22,0x11,  0x66,0x55,0x44,   /* ch0 s0, ch0 s1 */
		0xCC,0xBB,0xAA,  0xFF,0xEE,0xDD,   /* ch1 s0, ch1 s1 */
	};
	struct rtp_params p = { 97, 0, 0, 0 };
	uint8_t out[256];
	long n = rtp_l24_build(out, sizeof out, &p, planar, &TINY);
	ASSERT(n > 0);
	const uint8_t *a = out + RTP_HEADER_LEN;
	/* interleaved frame-major, big-endian:
	 *   s0c0=11 22 33, s0c1=AA BB CC, s1c0=44 55 66, s1c1=DD EE FF */
	uint8_t expect[12] = {
		0x11,0x22,0x33, 0xAA,0xBB,0xCC,
		0x44,0x55,0x66, 0xDD,0xEE,0xFF,
	};
	ASSERT(memcmp(a, expect, 12) == 0);
	return 0;
}

static int test_capacity_guard(void)
{
	uint8_t planar[2 * 2 * 3] = {0};
	struct rtp_params p = { 97, 0, 0, 0 };
	uint8_t out[10]; /* too small even for header */
	ASSERT_EQ_INT((int)rtp_l24_build(out, sizeof out, &p, planar, &TINY), -1);
	return 0;
}

static int test_full_48k_size(void)
{
	static uint8_t planar[REAC_MAX_CHANNELS * 24 * 3];
	struct rtp_params p = { 97, 0, 0, 0 };
	static uint8_t out[2048];
	long n = rtp_l24_build(out, sizeof out, &p, planar, &REAC_MODE_48K);
	ASSERT_EQ_INT(n, RTP_HEADER_LEN + 40 * 12 * 3); /* 12 + 1440 = 1452 */
	return 0;
}

static int test_build_flow_channel_slice(void)
{
	/* 4-channel planar block; extract the 2-channel slice [1,3) (ch1,ch2).
	 * Channels 0 and 3 are outside the flow and must NOT appear. */
	uint8_t planar[4 * 2 * 3] = {
		0x00,0x00,0x00, 0x00,0x00,0x00,   /* ch0 (excluded) */
		0x33,0x22,0x11, 0x66,0x55,0x44,   /* ch1 s0=0x112233, s1=0x445566 */
		0xCC,0xBB,0xAA, 0xFF,0xEE,0xDD,   /* ch2 s0=0xAABBCC, s1=0xDDEEFF */
		0x77,0x77,0x77, 0x77,0x77,0x77,   /* ch3 (excluded) */
	};
	struct rtp_params p = { 97, 0, 0, 0 };
	uint8_t out[256];
	long n = rtp_l24_build_flow(out, sizeof out, &p, planar,
	                            /*total_nch*/4, /*ch_start*/1, /*flow_nch*/2,
	                            /*n_samples*/2);
	ASSERT_EQ_INT(n, RTP_HEADER_LEN + 2 * 2 * 3); /* only the 2-ch slice */
	const uint8_t *a = out + RTP_HEADER_LEN;
	/* interleaved frame-major BE: s0c1, s0c2, s1c1, s1c2 */
	uint8_t expect[12] = {
		0x11,0x22,0x33, 0xAA,0xBB,0xCC,
		0x44,0x55,0x66, 0xDD,0xEE,0xFF,
	};
	ASSERT(memcmp(a, expect, 12) == 0);
	return 0;
}

static int test_build_flow_matches_full_build(void)
{
	/* build_flow over the whole channel set == rtp_l24_build (the special case). */
	uint8_t planar[2 * 2 * 3] = {
		0x33,0x22,0x11, 0x66,0x55,0x44,
		0xCC,0xBB,0xAA, 0xFF,0xEE,0xDD,
	};
	struct rtp_params p = { 97, 7, 12345, 0xAABBCCDD };
	uint8_t a[256], b[256];
	long na = rtp_l24_build(a, sizeof a, &p, planar, &TINY);
	long nb = rtp_l24_build_flow(b, sizeof b, &p, planar, 2, 0, 2, 2);
	ASSERT(na > 0 && na == nb);
	ASSERT(memcmp(a, b, (size_t)na) == 0);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_header_fields);
	rc |= RUN(test_planar_le_to_interleaved_be);
	rc |= RUN(test_capacity_guard);
	rc |= RUN(test_full_48k_size);
	rc |= RUN(test_build_flow_channel_slice);
	rc |= RUN(test_build_flow_matches_full_build);
	return rc;
}
