// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Pipeline stats accounting — the data the ubus status object publishes. */
#include <stdlib.h>
#include "../src/pipeline.h"
#include "../src/pcap_source.h"
#include <reac/reac.h>
#include "test_util.h"

static void noop_emit(void *ctx, const uint8_t *b, size_t l, int c)
{ (void)ctx; (void)b; (void)l; (void)c; }

static const char *fixture(void)
{
	const char *p = getenv("REAC_FIXTURE");
	return p ? p : "../reac-tools/tests/fixtures/real_reac_stream.pcap";
}

static int test_clean_run_counts_packets_no_loss(void)
{
	struct pipeline pl;
	pipeline_init(&pl, &REAC_MODE_48K, 97, 1);
	struct pcap_source ps;
	ASSERT_EQ_INT(pcap_source_open(&ps, fixture()), 0);
	uint8_t buf[2048]; long n;
	while ((n = pcap_source_next(&ps, buf, sizeof buf, NULL)) > 0)
		pipeline_on_frame(&pl, buf, (size_t)n, noop_emit, NULL);
	pcap_source_close(&ps);

	struct pipeline_stats s = pipeline_get_stats(&pl);
	ASSERT_EQ_INT((int)s.packets, 4);
	ASSERT_EQ_INT((int)s.concealed, 0);
	ASSERT_EQ_INT((int)s.loss_total, 0);
	ASSERT_EQ_INT((int)s.malformed, 0);
	ASSERT_EQ_INT(s.last_tier, -1); /* no loss yet */
	return 0;
}

static int test_loss_updates_counters_and_tier(void)
{
	struct pcap_source ps;
	ASSERT_EQ_INT(pcap_source_open(&ps, fixture()), 0);
	uint8_t f0[2048]; long n0 = pcap_source_next(&ps, f0, sizeof f0, NULL);
	pcap_source_close(&ps);
	ASSERT(n0 > 0);

	struct pipeline pl;
	pipeline_init(&pl, &REAC_MODE_48K, 97, 1);
	f0[14] = 100; f0[15] = 0;
	pipeline_on_frame(&pl, f0, (size_t)n0, noop_emit, NULL);
	f0[14] = 103; f0[15] = 0; /* 2 lost (101,102) */
	pipeline_on_frame(&pl, f0, (size_t)n0, noop_emit, NULL);

	struct pipeline_stats s = pipeline_get_stats(&pl);
	ASSERT_EQ_INT((int)s.packets, 2);     /* 2 real */
	ASSERT_EQ_INT((int)s.concealed, 2);   /* 2 fill */
	ASSERT_EQ_INT((int)s.loss_total, 2);
	/* first concealed packet of the run uses tier 0 (repeat+xfade) */
	ASSERT_EQ_INT(s.last_tier, PLC_TIER_BURST_FADE); /* 2nd fill = burst */
	return 0;
}

static int test_malformed_counted(void)
{
	struct pipeline pl;
	pipeline_init(&pl, &REAC_MODE_48K, 97, 1);
	uint8_t junk[100] = {0}; /* wrong length / no 0x8819 */
	int rc = pipeline_on_frame(&pl, junk, sizeof junk, noop_emit, NULL);
	ASSERT_EQ_INT(rc, -1);
	struct pipeline_stats s = pipeline_get_stats(&pl);
	ASSERT_EQ_INT((int)s.malformed, 1);
	ASSERT_EQ_INT((int)s.packets, 0);
	return 0;
}

static int test_plc_params_reach_plc(void)
{
	struct pipeline pl;
	/* custom crossfade width 5, custom burst fade 33 packets */
	pipeline_init_ex(&pl, &REAC_MODE_48K, 97, 1, 5, 33);
	ASSERT_EQ_INT(pl.plc.xfade_w, 5);
	ASSERT_EQ_INT(pl.plc.fade_packets, 33);
	/* default path keeps the built-in defaults (xfade min(S,8)=8, fade 200) */
	struct pipeline pd;
	pipeline_init(&pd, &REAC_MODE_48K, 97, 1);
	ASSERT_EQ_INT(pd.plc.xfade_w, 8);
	ASSERT_EQ_INT(pd.plc.fade_packets, 200);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_clean_run_counts_packets_no_loss);
	rc |= RUN(test_loss_updates_counters_and_tier);
	rc |= RUN(test_malformed_counted);
	rc |= RUN(test_plc_params_reach_plc);
	return rc;
}
