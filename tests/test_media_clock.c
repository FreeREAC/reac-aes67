// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "../src/media_clock.h"
#include "test_util.h"

static int test_first_packet_no_loss(void)
{
	struct media_clock mc;
	media_clock_init(&mc, 12);
	struct media_clock_step s = media_clock_on_packet(&mc, 100);
	ASSERT_EQ_INT(s.silence_packets, 0);
	ASSERT_EQ_INT(s.rtp_ts, 0);
	return 0;
}

static int test_consecutive_advances_by_samples(void)
{
	struct media_clock mc;
	media_clock_init(&mc, 12);
	media_clock_on_packet(&mc, 100);
	struct media_clock_step s = media_clock_on_packet(&mc, 101);
	ASSERT_EQ_INT(s.silence_packets, 0);
	ASSERT_EQ_INT(s.rtp_ts, 12); /* advanced one packet = 12 samples */
	struct media_clock_step s2 = media_clock_on_packet(&mc, 102);
	ASSERT_EQ_INT(s2.rtp_ts, 24);
	return 0;
}

static int test_gap_reports_silence_and_skips_timeline(void)
{
	struct media_clock mc;
	media_clock_init(&mc, 12);
	media_clock_on_packet(&mc, 100);          /* ts 0 */
	/* next expected 101, but we get 104 -> 3 lost packets */
	struct media_clock_step s = media_clock_on_packet(&mc, 104);
	ASSERT_EQ_INT(s.silence_packets, 3);
	/* 3 silence packets occupy ts 12,24,36; this packet lands at ts 48 */
	ASSERT_EQ_INT(s.rtp_ts, 48);
	ASSERT_EQ_INT((int)mc.total_lost, 3);
	return 0;
}

static int test_duplicate_counter_is_not_loss(void)
{
	struct media_clock mc;
	media_clock_init(&mc, 12);
	media_clock_on_packet(&mc, 100);                              /* ts 0 */
	struct media_clock_step s = media_clock_on_packet(&mc, 100);  /* duplicate */
	/* a dup must NOT be read as 65535 lost packets (unsigned-underflow trap) */
	ASSERT_EQ_INT(s.silence_packets, 0);
	ASSERT_EQ_INT(s.rtp_ts, 12);          /* timeline advances by exactly one packet */
	ASSERT_EQ_INT((int)mc.total_lost, 0);
	return 0;
}

static int test_reorder_does_not_explode_or_rewind(void)
{
	struct media_clock mc;
	media_clock_init(&mc, 12);
	media_clock_on_packet(&mc, 100);                                /* ts 0 */
	media_clock_on_packet(&mc, 101);                                /* ts 12, consecutive */
	struct media_clock_step late = media_clock_on_packet(&mc, 99);  /* reordered / late */
	ASSERT_EQ_INT(late.silence_packets, 0);                         /* not 65533 */
	ASSERT_EQ_INT(late.rtp_ts, 24);
	/* the late packet must NOT rewind last_counter, or the next in-order packet
	 * (102) would look like a fresh loss */
	struct media_clock_step nxt = media_clock_on_packet(&mc, 102);
	ASSERT_EQ_INT(nxt.silence_packets, 0);
	ASSERT_EQ_INT(nxt.rtp_ts, 36);
	ASSERT_EQ_INT((int)mc.total_lost, 0);
	return 0;
}

static int test_counter_wraps(void)
{
	struct media_clock mc;
	media_clock_init(&mc, 12);
	media_clock_on_packet(&mc, 0xffff);
	struct media_clock_step s = media_clock_on_packet(&mc, 0x0000); /* wrap, consecutive */
	ASSERT_EQ_INT(s.silence_packets, 0);
	ASSERT_EQ_INT(s.rtp_ts, 12);
	return 0;
}

static int test_anchored_first_packet_starts_at_anchor(void)
{
	/* PTP-locked (Dante) mode: the first packet's RTP ts is the TAI-derived
	 * anchor, not 0; subsequent packets advance from it as usual. */
	struct media_clock mc;
	media_clock_init_anchored(&mc, 12, 1000000u);
	struct media_clock_step s = media_clock_on_packet(&mc, 100);
	ASSERT_EQ_INT(s.rtp_ts, 1000000);
	struct media_clock_step s2 = media_clock_on_packet(&mc, 101);
	ASSERT_EQ_INT(s2.rtp_ts, 1000012);
	return 0;
}

static int test_96k_uses_24_samples(void)
{
	struct media_clock mc;
	media_clock_init(&mc, 24);
	media_clock_on_packet(&mc, 0);
	struct media_clock_step s = media_clock_on_packet(&mc, 1);
	ASSERT_EQ_INT(s.rtp_ts, 24);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_first_packet_no_loss);
	rc |= RUN(test_consecutive_advances_by_samples);
	rc |= RUN(test_gap_reports_silence_and_skips_timeline);
	rc |= RUN(test_duplicate_counter_is_not_loss);
	rc |= RUN(test_reorder_does_not_explode_or_rewind);
	rc |= RUN(test_counter_wraps);
	rc |= RUN(test_anchored_first_packet_starts_at_anchor);
	rc |= RUN(test_96k_uses_24_samples);
	return rc;
}
