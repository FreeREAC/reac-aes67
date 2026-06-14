// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* End-to-end loopback: fixture pcap -> pipeline -> aes67_send (UDP 127.0.0.1)
 * -> receive -> assert the expected number of RTP L24 packets arrive with the
 * right seq progression and payload size. Proves the whole M2 path off-hardware
 * (only the AF_PACKET capture is excluded). CI-safe, no privileges. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "../src/pipeline.h"
#include "../src/pcap_source.h"
#include "../src/aes67_send.h"
#include "../src/reac_mode.h"
#include "test_util.h"

static const char *fixture(void)
{
	const char *p = getenv("REAC_FIXTURE");
	return p ? p : "../reac-tools/tests/fixtures/real_reac_stream.pcap";
}

struct sink { struct aes67_sender s; };
static void emit_udp(void *ctx, const uint8_t *buf, size_t len, int concealed)
{
	(void)concealed;
	aes67_send_packet(&((struct sink *)ctx)->s, buf, len);
}

static int test_fixture_to_udp_roundtrip(void)
{
	/* receiver on ephemeral loopback port */
	int rx = socket(AF_INET, SOCK_DGRAM, 0);
	ASSERT(rx >= 0);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
	ASSERT_EQ_INT(bind(rx, (struct sockaddr *)&a, sizeof a), 0);
	socklen_t al = sizeof a;
	getsockname(rx, (struct sockaddr *)&a, &al);
	uint16_t port = ntohs(a.sin_port);

	struct sink sk;
	ASSERT_EQ_INT(aes67_send_open(&sk.s, "127.0.0.1", port, 1, NULL), 0);

	/* run the fixture (4 consecutive frames -> 4 RTP packets, no loss) */
	struct pipeline pl;
	pipeline_init(&pl, &REAC_MODE_48K, 97, 0xCAFEBABE);
	struct pcap_source ps;
	ASSERT_EQ_INT(pcap_source_open(&ps, fixture()), 0);
	uint8_t buf[2048];
	long n;
	while ((n = pcap_source_next(&ps, buf, sizeof buf, NULL)) > 0)
		pipeline_on_frame(&pl, buf, (size_t)n, emit_udp, &sk);
	pcap_source_close(&ps);
	aes67_send_close(&sk.s);

	/* drain the receiver (non-blocking) */
	int fl = fcntl(rx, F_GETFL, 0); fcntl(rx, F_SETFL, fl | O_NONBLOCK);
	uint8_t pkt[2048];
	int got = 0; uint16_t first_seq = 0, last_seq = 0;
	for (;;) {
		long r = recv(rx, pkt, sizeof pkt, 0);
		if (r < 0) break;
		ASSERT_EQ_INT((int)r, RTP_HEADER_LEN + 40 * 12 * 3); /* L24 48k/40ch */
		uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
		if (got == 0) first_seq = seq;
		last_seq = seq;
		got++;
	}
	close(rx);

	ASSERT_EQ_INT(got, 4);
	ASSERT_EQ_INT(last_seq, (uint16_t)(first_seq + 3));
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_fixture_to_udp_roundtrip);
	return rc;
}
