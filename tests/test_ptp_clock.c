// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* tai_to_rtp_ts: RFC 7273 mediaclk:direct=0 timestamp derivation.
 * RTP_ts = ( floor(TAI_seconds*Fs) + round(TAI_nanos*Fs/1e9) ) mod 2^32. */
#include "../src/ptp_clock.h"
#include "test_util.h"

static int test_epoch_is_zero(void)
{
	ASSERT_EQ_INT(tai_to_rtp_ts(0, 0, 48000), 0);
	return 0;
}

static int test_one_second_is_one_rate(void)
{
	ASSERT_EQ_INT(tai_to_rtp_ts(1, 0, 48000), 48000);
	ASSERT_EQ_INT(tai_to_rtp_ts(2, 0, 48000), 96000);
	return 0;
}

static int test_half_second(void)
{
	/* 0.5 s at 48 kHz = 24000 samples */
	ASSERT_EQ_INT(tai_to_rtp_ts(0, 500000000u, 48000), 24000);
	return 0;
}

static int test_subsample_rounds_to_nearest(void)
{
	/* ~half a sample period (20833.33 ns) rounds up to 1 */
	ASSERT_EQ_INT(tai_to_rtp_ts(0, 10417u, 48000), 1);
	/* well under half rounds to 0 */
	ASSERT_EQ_INT(tai_to_rtp_ts(0, 1000u, 48000), 0);
	return 0;
}

static int test_wraps_at_2_pow_32(void)
{
	/* 2^32 / 48000 = 89478.485..s; at 89479 s the sample count exceeds 2^32.
	 * 89479*48000 = 4294992000; mod 2^32 (4294967296) = 24704. */
	ASSERT_EQ_INT(tai_to_rtp_ts(89479u, 0, 48000), 24704);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_epoch_is_zero);
	rc |= RUN(test_one_second_is_one_rate);
	rc |= RUN(test_half_second);
	rc |= RUN(test_subsample_rounds_to_nearest);
	rc |= RUN(test_wraps_at_2_pow_32);
	return rc;
}
