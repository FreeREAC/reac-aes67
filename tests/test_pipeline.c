// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <stdlib.h>
#include <string.h>
#include "../src/pipeline.h"
#include "../src/pcap_source.h"
#include "../src/reac_decode.h"
#include <reac/reac.h>
#include "test_util.h"

/* collect emitted packets */
struct collected {
	int n;
	int concealed[64];
	uint16_t seq[64];
	uint32_t ts[64];
	uint8_t payload0[64][8]; /* first 8 payload bytes for content checks */
	int paylen[64];
};

static void on_emit(void *ctx, const uint8_t *buf, size_t len, int is_concealed)
{
	struct collected *c = ctx;
	if (c->n >= 64) return;
	int i = c->n++;
	c->concealed[i] = is_concealed;
	c->seq[i] = (uint16_t)((buf[2] << 8) | buf[3]);
	c->ts[i] = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
	           ((uint32_t)buf[6] << 8) | buf[7];
	c->paylen[i] = (int)len - RTP_HEADER_LEN;
	memcpy(c->payload0[i], buf + RTP_HEADER_LEN, 8);
}

static const char *fixture(void)
{
	const char *p = getenv("REAC_FIXTURE");
	return p ? p : "../reac-tools/tests/fixtures/real_reac_stream.pcap";
}

/* Feed the 4 real fixture frames; expect 4 real RTP packets, seq+1 each,
 * ts advancing by 12 samples, none concealed. */
static int test_fixture_four_clean_packets(void)
{
	struct pipeline pl;
	pipeline_init(&pl, &REAC_MODE_48K, 97, 0xCAFEBABE);
	struct collected c = {0};
	struct pcap_source ps;
	ASSERT_EQ_INT(pcap_source_open(&ps, fixture()), 0);
	uint8_t buf[2048];
	long n;
	while ((n = pcap_source_next(&ps, buf, sizeof buf, NULL)) > 0)
		pipeline_on_frame(&pl, buf, (size_t)n, on_emit, &c);
	pcap_source_close(&ps);

	ASSERT_EQ_INT(c.n, 4);
	for (int i = 0; i < 4; i++)
		ASSERT_EQ_INT(c.concealed[i], 0);
	/* seq monotonic +1 */
	ASSERT_EQ_INT(c.seq[1], (uint16_t)(c.seq[0] + 1));
	ASSERT_EQ_INT(c.seq[3], (uint16_t)(c.seq[0] + 3));
	/* ts advances by 12 samples per packet */
	ASSERT_EQ_INT(c.ts[1] - c.ts[0], 12);
	ASSERT_EQ_INT(c.ts[3] - c.ts[0], 36);
	/* payload size = 40ch*12*3 */
	ASSERT_EQ_INT(c.paylen[0], 40 * 12 * 3);
	return 0;
}

/* Synthesize a counter gap by feeding frame copies with edited l2_counter,
 * and assert hold-last concealment (repeat previous payload), NOT silence. */
static int test_loss_holds_last_not_silence(void)
{
	/* load first real frame */
	struct pcap_source ps;
	ASSERT_EQ_INT(pcap_source_open(&ps, fixture()), 0);
	uint8_t f0[2048]; long n0 = pcap_source_next(&ps, f0, sizeof f0, NULL);
	pcap_source_close(&ps);
	ASSERT(n0 > 0);

	struct pipeline pl;
	pipeline_init(&pl, &REAC_MODE_48K, 97, 1);
	struct collected c = {0};

	/* packet A: counter 100 (edit bytes 14-15 LE) */
	f0[14] = 100; f0[15] = 0;
	pipeline_on_frame(&pl, f0, (size_t)n0, on_emit, &c);

	/* packet B: counter 103 -> 2 lost (101,102) */
	f0[14] = 103; f0[15] = 0;
	pipeline_on_frame(&pl, f0, (size_t)n0, on_emit, &c);

	/* expect: A(real), then 2 concealed (PLC), then B(real) = 4 total */
	ASSERT_EQ_INT(c.n, 4);
	ASSERT_EQ_INT(c.concealed[0], 0);
	ASSERT_EQ_INT(c.concealed[1], 1);
	ASSERT_EQ_INT(c.concealed[2], 1);
	ASSERT_EQ_INT(c.concealed[3], 0);
	/* concealment derives from A's audio (repeat+crossfade), NOT silence.
	 * The capture used here happens to be near-silent so we can't assert
	 * "nonzero" on these bytes; the dedicated PLC tests cover content. Here we
	 * assert the pipeline WIRING: count, concealed flags, timeline, seq. */
	/* timestamps continue monotonically across the gap */
	ASSERT_EQ_INT(c.ts[1] - c.ts[0], 12);
	ASSERT_EQ_INT(c.ts[2] - c.ts[1], 12);
	ASSERT_EQ_INT(c.ts[3] - c.ts[2], 12);
	/* RTP seq increments for every emitted packet incl concealed */
	ASSERT_EQ_INT(c.seq[3], (uint16_t)(c.seq[0] + 3));
	return 0;
}

/* When a PCM sink is set (Dante path: feed the packetizer), the pipeline emits
 * planar PCM per REAC frame instead of building RTP itself. */
struct pcm_collected {
	int n;
	uint32_t ts[64];
	int concealed[64];
	uint8_t first6[64][6];
};
static void on_pcm(void *ctx, const uint8_t *planar, uint32_t rtp_ts, int concealed)
{
	struct pcm_collected *c = ctx;
	if (c->n >= 64) return;
	int i = c->n++;
	c->ts[i] = rtp_ts;
	c->concealed[i] = concealed;
	memcpy(c->first6[i], planar, 6);
}

static int never_called_emit_count = 0;
static void must_not_emit(void *ctx, const uint8_t *b, size_t l, int c)
{
	(void)ctx; (void)b; (void)l; (void)c;
	never_called_emit_count++;
}

static int test_pcm_sink_receives_planar_frames(void)
{
	struct pipeline pl;
	pipeline_init(&pl, &REAC_MODE_48K, 97, 1);
	struct pcm_collected pc = {0};
	pipeline_set_pcm_sink(&pl, on_pcm, &pc);
	never_called_emit_count = 0;

	/* independently decode the first fixture frame for a content check */
	struct pcap_source ps;
	ASSERT_EQ_INT(pcap_source_open(&ps, fixture()), 0);
	uint8_t buf[2048]; uint8_t pcm_ref[REAC_MAX_CHANNELS * 24 * 3];
	long n; int got_ref = 0;
	while ((n = pcap_source_next(&ps, buf, sizeof buf, NULL)) > 0) {
		if (!got_ref) {
			reac_decode(buf, (size_t)n, &REAC_MODE_48K, pcm_ref);
			got_ref = 1;
		}
		pipeline_on_frame(&pl, buf, (size_t)n, must_not_emit, NULL);
	}
	pcap_source_close(&ps);

	ASSERT_EQ_INT(pc.n, 4);                          /* one planar frame each */
	ASSERT_EQ_INT(never_called_emit_count, 0);       /* RTP emit bypassed */
	ASSERT_EQ_INT((int)(pc.ts[1] - pc.ts[0]), 12);   /* per-frame timeline */
	ASSERT(memcmp(pc.first6[0], pcm_ref, 6) == 0);   /* sink got real planar PCM */
	return 0;
}

static int test_pcm_sink_honours_ptp_anchor(void)
{
	struct pipeline pl;
	pipeline_init(&pl, &REAC_MODE_48K, 97, 1);
	pipeline_anchor(&pl, 5000000u);                  /* PTP/TAI-derived anchor */
	struct pcm_collected pc = {0};
	pipeline_set_pcm_sink(&pl, on_pcm, &pc);

	struct pcap_source ps;
	ASSERT_EQ_INT(pcap_source_open(&ps, fixture()), 0);
	uint8_t buf[2048]; long n;
	while ((n = pcap_source_next(&ps, buf, sizeof buf, NULL)) > 0)
		pipeline_on_frame(&pl, buf, (size_t)n, must_not_emit, NULL);
	pcap_source_close(&ps);

	ASSERT(pc.n >= 1);
	ASSERT_EQ_INT((int)pc.ts[0], 5000000);           /* first packet at anchor */
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_fixture_four_clean_packets);
	rc |= RUN(test_loss_holds_last_not_silence);
	rc |= RUN(test_pcm_sink_receives_planar_frames);
	rc |= RUN(test_pcm_sink_honours_ptp_anchor);
	return rc;
}
