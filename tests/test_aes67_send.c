// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Loopback test: open a sender to 127.0.0.1:PORT, receive on a UDP socket,
 * assert the exact bytes round-trip. No privileges, CI-safe. */
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "../src/aes67_send.h"
#include "test_util.h"

static int test_loopback_roundtrip(void)
{
	/* receiver socket on an ephemeral port */
	int rx = socket(AF_INET, SOCK_DGRAM, 0);
	ASSERT(rx >= 0);
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	ASSERT_EQ_INT(bind(rx, (struct sockaddr *)&addr, sizeof addr), 0);
	socklen_t al = sizeof addr;
	ASSERT_EQ_INT(getsockname(rx, (struct sockaddr *)&addr, &al), 0);
	uint16_t port = ntohs(addr.sin_port);

	struct aes67_sender s;
	ASSERT_EQ_INT(aes67_send_open(&s, "127.0.0.1", port, 1, NULL), 0);

	uint8_t pkt[64];
	for (int i = 0; i < 64; i++) pkt[i] = (uint8_t)(i * 7 + 1);
	long sent = aes67_send_packet(&s, pkt, sizeof pkt);
	ASSERT_EQ_INT((int)sent, 64);

	uint8_t got[128];
	long n = recv(rx, got, sizeof got, 0);
	ASSERT_EQ_INT((int)n, 64);
	ASSERT(memcmp(got, pkt, 64) == 0);

	aes67_send_close(&s);
	close(rx);
	return 0;
}

static int test_open_bad_addr_fails(void)
{
	struct aes67_sender s;
	ASSERT_EQ_INT(aes67_send_open(&s, "not.an.ip", 5004, 1, NULL), -1);
	return 0;
}

static int test_set_dscp_marks_af41(void)
{
	struct aes67_sender s;
	ASSERT_EQ_INT(aes67_send_open(&s, "127.0.0.1", 5004, 1, NULL), 0);
	ASSERT_EQ_INT(aes67_send_set_dscp(&s, 34), 0);   /* AF41 */
	int tos = 0; socklen_t tl = sizeof tos;
	ASSERT_EQ_INT(getsockopt(s.fd, IPPROTO_IP, IP_TOS, &tos, &tl), 0);
	ASSERT_EQ_INT(tos, 34 << 2);   /* DSCP in the high 6 bits of the TOS byte */
	aes67_send_close(&s);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_loopback_roundtrip);
	rc |= RUN(test_open_bad_addr_fails);
	rc |= RUN(test_set_dscp_marks_af41);
	return rc;
}
