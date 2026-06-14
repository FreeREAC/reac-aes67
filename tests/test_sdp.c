// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <string.h>
#include "../src/sdp.h"
#include "test_util.h"

static int test_sdp_contains_required_lines(void)
{
	struct sdp_params p = {
		.session_name = "REAC-A", .origin_ip = "10.0.0.1",
		.mcast_ip = "239.69.0.1", .mcast_port = 5004,
		.payload_type = 97, .rate = 48000, .channels = 40, .ttl = 1,
	};
	char out[1024];
	long n = sdp_build(out, sizeof out, &p);
	ASSERT(n > 0);
	ASSERT(strstr(out, "v=0") != NULL);
	ASSERT(strstr(out, "s=REAC-A") != NULL);
	/* connection: multicast group with TTL */
	ASSERT(strstr(out, "c=IN IP4 239.69.0.1/1") != NULL);
	/* media: audio port, RTP/AVP, dynamic PT */
	ASSERT(strstr(out, "m=audio 5004 RTP/AVP 97") != NULL);
	/* rtpmap: L24, rate, channels */
	ASSERT(strstr(out, "a=rtpmap:97 L24/48000/40") != NULL);
	return 0;
}

static int test_sdp_96k_20ch(void)
{
	struct sdp_params p = {
		.session_name = "REAC-B", .origin_ip = "10.0.0.1",
		.mcast_ip = "239.69.0.2", .mcast_port = 5006,
		.payload_type = 98, .rate = 96000, .channels = 20, .ttl = 2,
	};
	char out[1024];
	ASSERT(sdp_build(out, sizeof out, &p) > 0);
	ASSERT(strstr(out, "a=rtpmap:98 L24/96000/20") != NULL);
	ASSERT(strstr(out, "c=IN IP4 239.69.0.2/2") != NULL);
	return 0;
}

static int test_sdp_capacity_guard(void)
{
	struct sdp_params p = {
		.session_name = "X", .origin_ip = "1.1.1.1", .mcast_ip = "239.0.0.1",
		.mcast_port = 5004, .payload_type = 97, .rate = 48000, .channels = 40, .ttl = 1,
	};
	char tiny[10];
	ASSERT_EQ_INT((int)sdp_build(tiny, sizeof tiny, &p), -1);
	return 0;
}

static int test_sdp_dante_rfc7273(void)
{
	/* a Dante AES67 flow SDP: L24/48000/8, ptime 1ms, RFC 7273 clock signalling */
	struct sdp_params p = {
		.session_name = "REAC-A-1", .origin_ip = "10.0.0.1",
		.mcast_ip = "239.69.10.0", .mcast_port = 5004,
		.payload_type = 96, .rate = 48000, .channels = 8, .ttl = 32,
		.ptime_ms = 1,
		.ts_refclk = "00-11-22-FF-FE-33-44-55:0",
		.mediaclk_direct = 0,
	};
	char out[1024];
	ASSERT(sdp_build(out, sizeof out, &p) > 0);
	ASSERT(strstr(out, "a=rtpmap:96 L24/48000/8") != NULL);
	ASSERT(strstr(out, "a=ptime:1") != NULL);
	ASSERT(strstr(out,
	       "a=ts-refclk:ptp=IEEE1588-2008:00-11-22-FF-FE-33-44-55:0") != NULL);
	ASSERT(strstr(out, "a=mediaclk:direct=0") != NULL);
	return 0;
}

static int test_sdp_aes67_omits_refclk(void)
{
	/* plain AES67 (no ts_refclk): must NOT carry RFC 7273 lines */
	struct sdp_params p = {
		.session_name = "REAC-A", .origin_ip = "10.0.0.1",
		.mcast_ip = "239.69.0.1", .mcast_port = 5004,
		.payload_type = 97, .rate = 48000, .channels = 40, .ttl = 1,
	};
	char out[1024];
	ASSERT(sdp_build(out, sizeof out, &p) > 0);
	ASSERT(strstr(out, "ts-refclk") == NULL);
	ASSERT(strstr(out, "mediaclk") == NULL);
	ASSERT(strstr(out, "a=ptime:1") != NULL); /* default ptime unchanged */
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_sdp_contains_required_lines);
	rc |= RUN(test_sdp_96k_20ch);
	rc |= RUN(test_sdp_capacity_guard);
	rc |= RUN(test_sdp_dante_rfc7273);
	rc |= RUN(test_sdp_aes67_omits_refclk);
	return rc;
}
