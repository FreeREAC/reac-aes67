// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* packetizer: aggregate frames_per_packet decoded REAC frames into ONE RTP
 * packet per flow, splitting the channel set into <=8-ch Dante flows. */
#include <string.h>
#include "../src/packetizer.h"
#include "../src/rtp_l24.h"
#include <reac/reac.h>
#include "test_util.h"

/* small hand-checkable mode: 4 channels, 2 samples/frame */
static const struct reac_mode M4x2 = { 48000, 4, 2 };

struct cap {
	int n;
	int flow_idx[64];
	size_t len[64];
	uint32_t ts[64];
	uint32_t ssrc[64];
	uint16_t seq[64];
	int concealed[64];
	uint8_t payload[64][64];
};

static void on_emit(void *ctx, int flow_index, const uint8_t *buf, size_t len,
                    int concealed)
{
	struct cap *c = ctx;
	int i = c->n++;
	c->flow_idx[i] = flow_index;
	c->len[i] = len;
	c->ts[i] = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
	           ((uint32_t)buf[6] << 8) | buf[7];
	c->ssrc[i] = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
	             ((uint32_t)buf[10] << 8) | buf[11];
	c->seq[i] = (uint16_t)((buf[2] << 8) | buf[3]);
	c->concealed[i] = concealed;
	size_t plen = len - RTP_HEADER_LEN;
	if (plen > sizeof c->payload[i]) plen = sizeof c->payload[i];
	memcpy(c->payload[i], buf + RTP_HEADER_LEN, plen);
}

/* build a per-frame planar block where channel c, sample s holds the 24-bit
 * value (frame*100 + c*10 + s), little-endian, so we can trace placement. */
static void make_frame(uint8_t *planar, int frame, int nch, int nsamp)
{
	for (int c = 0; c < nch; c++)
		for (int s = 0; s < nsamp; s++) {
			uint32_t v = (uint32_t)(frame * 100 + c * 10 + s);
			uint8_t *p = planar + (size_t)(c * nsamp + s) * 3;
			p[0] = (uint8_t)(v & 0xff);
			p[1] = (uint8_t)((v >> 8) & 0xff);
			p[2] = (uint8_t)((v >> 16) & 0xff);
		}
}

/* read a big-endian 24-bit payload sample at (output_sample gs, flow-channel fc) */
static uint32_t pl_be24(const uint8_t *payload, int gs, int flow_nch, int fc)
{
	const uint8_t *p = payload + (size_t)(gs * flow_nch + fc) * 3;
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static int test_fpp1_single_flow_passthrough(void)
{
	struct reac_flow flows[1] = { { .ch_start = 0, .n_ch = 4, .payload_type = 97,
	                                .ssrc = 0xAA, .seq = 0 } };
	struct packetizer pk;
	packetizer_init(&pk, &M4x2, 1, flows, 1);

	uint8_t f0[4 * 2 * 3];
	make_frame(f0, 0, 4, 2);
	struct cap c = {0};
	packetizer_push(&pk, f0, 5000, 0, on_emit, &c);
	ASSERT_EQ_INT(c.n, 1);                       /* FPP=1 -> emit immediately */
	ASSERT_EQ_INT((int)c.len[0], RTP_HEADER_LEN + 4 * 2 * 3);
	ASSERT_EQ_INT((int)c.ts[0], 5000);
	ASSERT_EQ_INT((int)c.seq[0], 0);

	uint8_t f1[4 * 2 * 3];
	make_frame(f1, 1, 4, 2);
	packetizer_push(&pk, f1, 5002, 0, on_emit, &c);
	ASSERT_EQ_INT(c.n, 2);
	ASSERT_EQ_INT((int)c.seq[1], 1);             /* seq advances per packet */
	return 0;
}

static int test_fpp2_aggregates_two_frames(void)
{
	struct reac_flow flows[1] = { { .ch_start = 0, .n_ch = 4, .payload_type = 97,
	                                .ssrc = 0xBB, .seq = 0 } };
	struct packetizer pk;
	packetizer_init(&pk, &M4x2, 2, flows, 1);

	uint8_t f0[4 * 2 * 3], f1[4 * 2 * 3];
	make_frame(f0, 0, 4, 2);
	make_frame(f1, 1, 4, 2);
	struct cap c = {0};

	packetizer_push(&pk, f0, 9000, 0, on_emit, &c);
	ASSERT_EQ_INT(c.n, 0);                       /* not full yet */
	packetizer_push(&pk, f1, 9002, 0, on_emit, &c);
	ASSERT_EQ_INT(c.n, 1);                       /* full at 2 frames */
	ASSERT_EQ_INT((int)c.len[0], RTP_HEADER_LEN + 4 * 4 * 3); /* 4ch x 4 samp */
	ASSERT_EQ_INT((int)c.ts[0], 9000);           /* timestamp = FIRST frame */

	/* output samples 0,1 come from frame0; samples 2,3 from frame1 (time order) */
	ASSERT_EQ_INT((int)pl_be24(c.payload[0], 0, 4, 0), 0);   /* f0 c0 s0 = 0 */
	ASSERT_EQ_INT((int)pl_be24(c.payload[0], 1, 4, 0), 1);   /* f0 c0 s1 = 1 */
	ASSERT_EQ_INT((int)pl_be24(c.payload[0], 2, 4, 0), 100); /* f1 c0 s0 = 100 */
	ASSERT_EQ_INT((int)pl_be24(c.payload[0], 3, 4, 2), 121); /* f1 c2 s1 = 121 */
	return 0;
}

static int test_flow_split_two_flows(void)
{
	struct reac_flow flows[2] = {
		{ .ch_start = 0, .n_ch = 2, .payload_type = 97, .ssrc = 0x111, .seq = 0 },
		{ .ch_start = 2, .n_ch = 2, .payload_type = 97, .ssrc = 0x222, .seq = 0 },
	};
	struct packetizer pk;
	packetizer_init(&pk, &M4x2, 1, flows, 2);

	uint8_t f0[4 * 2 * 3];
	make_frame(f0, 0, 4, 2);
	struct cap c = {0};
	packetizer_push(&pk, f0, 7000, 0, on_emit, &c);
	ASSERT_EQ_INT(c.n, 2);                        /* one packet per flow */
	ASSERT_EQ_INT(c.flow_idx[0], 0);
	ASSERT_EQ_INT(c.flow_idx[1], 1);
	ASSERT_EQ_INT((int)c.ssrc[0], 0x111);
	ASSERT_EQ_INT((int)c.ssrc[1], 0x222);
	ASSERT_EQ_INT((int)c.len[0], RTP_HEADER_LEN + 2 * 2 * 3);
	/* flow0 carries channels 0,1 */
	ASSERT_EQ_INT((int)pl_be24(c.payload[0], 0, 2, 0), 0);   /* c0 s0 */
	ASSERT_EQ_INT((int)pl_be24(c.payload[0], 0, 2, 1), 10);  /* c1 s0 = 10 */
	/* flow1 carries channels 2,3 */
	ASSERT_EQ_INT((int)pl_be24(c.payload[1], 0, 2, 0), 20);  /* c2 s0 = 20 */
	ASSERT_EQ_INT((int)pl_be24(c.payload[1], 0, 2, 1), 30);  /* c3 s0 = 30 */
	return 0;
}

static int test_dante_40ch_5x8_at_1ms(void)
{
	/* the headline shape: 40 ch, 12 samp/frame, FPP=4 (1 ms = 48 samp),
	 * 5 flows of 8 channels. 4 pushes -> 5 packets of 8ch x 48 samp. */
	struct reac_flow flows[5];
	for (int i = 0; i < 5; i++)
		flows[i] = (struct reac_flow){ .ch_start = i * 8, .n_ch = 8,
		                               .payload_type = 96,
		                               .ssrc = 0x1000u + (uint32_t)i, .seq = 0 };
	struct packetizer pk;
	packetizer_init(&pk, &REAC_MODE_48K, 4, flows, 5);

	static uint8_t frame[REAC_MAX_CHANNELS * 12 * 3];
	struct cap c = {0};
	for (int f = 0; f < 4; f++) {
		make_frame(frame, f, 40, 12);
		packetizer_push(&pk, frame, 20000u + (uint32_t)f * 12u, 0, on_emit, &c);
	}
	ASSERT_EQ_INT(c.n, 5);                        /* 5 flows, one packet each */
	for (int i = 0; i < 5; i++) {
		ASSERT_EQ_INT(c.flow_idx[i], i);
		ASSERT_EQ_INT((int)c.len[i], RTP_HEADER_LEN + 8 * 48 * 3); /* 1164 */
		ASSERT_EQ_INT((int)c.ts[i], 20000);       /* first frame's ts */
		ASSERT_EQ_INT((int)c.ssrc[i], 0x1000 + i);
	}
	return 0;
}

static int test_concealed_flag_propagates(void)
{
	struct reac_flow flows[1] = { { .ch_start = 0, .n_ch = 4, .payload_type = 97,
	                                .ssrc = 1, .seq = 0 } };
	struct packetizer pk;
	packetizer_init(&pk, &M4x2, 2, flows, 1);
	uint8_t f[4 * 2 * 3];
	make_frame(f, 0, 4, 2);
	struct cap c = {0};
	packetizer_push(&pk, f, 0, 0, on_emit, &c);   /* real */
	packetizer_push(&pk, f, 2, 1, on_emit, &c);   /* concealed -> packet concealed */
	ASSERT_EQ_INT(c.n, 1);
	ASSERT_EQ_INT(c.concealed[0], 1);
	return 0;
}

static int test_build_flows_40ch_8wide(void)
{
	struct reac_flow flows[8];
	int n = dante_build_flows(flows, 8, 40, 8, 96, 0x5000);
	ASSERT_EQ_INT(n, 5);                          /* 40 / 8 = 5 flows */
	for (int i = 0; i < 5; i++) {
		ASSERT_EQ_INT(flows[i].ch_start, i * 8);
		ASSERT_EQ_INT(flows[i].n_ch, 8);
		ASSERT_EQ_INT(flows[i].payload_type, 96);
		ASSERT_EQ_INT((int)flows[i].ssrc, 0x5000 + i);
		ASSERT_EQ_INT((int)flows[i].seq, 0);
	}
	return 0;
}

static int test_build_flows_remainder(void)
{
	struct reac_flow flows[8];
	int n = dante_build_flows(flows, 8, 40, 12, 96, 0);
	ASSERT_EQ_INT(n, 4);                          /* 12,12,12,4 */
	ASSERT_EQ_INT(flows[3].ch_start, 36);
	ASSERT_EQ_INT(flows[3].n_ch, 4);
	return 0;
}

static int test_build_flows_max_guard(void)
{
	struct reac_flow flows[2];
	ASSERT_EQ_INT(dante_build_flows(flows, 2, 40, 8, 96, 0), -1); /* needs 5 > 2 */
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_fpp1_single_flow_passthrough);
	rc |= RUN(test_fpp2_aggregates_two_frames);
	rc |= RUN(test_flow_split_two_flows);
	rc |= RUN(test_dante_40ch_5x8_at_1ms);
	rc |= RUN(test_concealed_flag_propagates);
	rc |= RUN(test_build_flows_40ch_8wide);
	rc |= RUN(test_build_flows_remainder);
	rc |= RUN(test_build_flows_max_guard);
	return rc;
}
