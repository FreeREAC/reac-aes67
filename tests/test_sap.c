// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <string.h>
#include "../src/sap.h"
#include "test_util.h"

static const char *SDP = "v=0\r\ns=REAC-A\r\n";

static int test_header_and_mime(void)
{
	uint8_t out[256];
	long n = sap_build(out, sizeof out, "10.0.0.1", 0xBEEF, SDP);
	ASSERT(n > 0);
	/* SAP header */
	ASSERT_EQ_INT(out[0], 0x20);  /* V=1, announce, IPv4 */
	ASSERT_EQ_INT(out[1], 0x00);  /* auth len 0 */
	ASSERT_EQ_INT(out[2], 0xBE);  /* msg id hash hi */
	ASSERT_EQ_INT(out[3], 0xEF);  /* msg id hash lo */
	ASSERT_EQ_INT(out[4], 10);    /* origin IPv4 net order */
	ASSERT_EQ_INT(out[5], 0);
	ASSERT_EQ_INT(out[6], 0);
	ASSERT_EQ_INT(out[7], 1);
	/* MIME type follows, NUL-terminated */
	ASSERT(memcmp(out + 8, "application/sdp", 15) == 0);
	ASSERT_EQ_INT(out[8 + 15], 0x00);
	/* then the SDP body verbatim */
	ASSERT(memcmp(out + 8 + 16, SDP, strlen(SDP)) == 0);
	ASSERT_EQ_INT((int)n, 8 + 16 + (int)strlen(SDP));
	return 0;
}

static int test_capacity_guard(void)
{
	uint8_t out[10];
	ASSERT_EQ_INT((int)sap_build(out, sizeof out, "10.0.0.1", 1, SDP), -1);
	return 0;
}

static int test_bad_origin(void)
{
	uint8_t out[256];
	ASSERT_EQ_INT((int)sap_build(out, sizeof out, "not.an.ip", 1, SDP), -1);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_header_and_mime);
	rc |= RUN(test_capacity_guard);
	rc |= RUN(test_bad_origin);
	return rc;
}
