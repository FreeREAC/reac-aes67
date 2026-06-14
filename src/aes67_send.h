// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* aes67_send — UDP (multicast) transmit for RTP L24 packets.
 *
 * Thin socket shell: the RTP packet bytes are built by rtp_l24 (tested
 * separately); this just opens a UDP socket to a destination group:port and
 * sends pre-built datagrams. Works to a multicast group or a plain unicast
 * address (e.g. 127.0.0.1 for loopback tests).
 */
#ifndef AES67_SEND_H
#define AES67_SEND_H

#include <stdint.h>
#include <stddef.h>

struct aes67_sender {
	int fd;
	void *dest;        /* struct sockaddr_in *, opaque here */
};

/* Open a sender to dotted-quad ip:port. ttl applies to multicast (1 = local
 * subnet). ifname, if non-NULL/non-empty, pins multicast egress to that
 * interface (IP_MULTICAST_IF by its address) so announcements/RTP leave the
 * intended segment instead of following the default route (e.g. out WAN on a
 * router). Returns 0 on success, -1 on error. */
int aes67_send_open(struct aes67_sender *s, const char *ip, uint16_t port,
                    int ttl, const char *ifname);

/* Mark egress packets with a DSCP class via IP_TOS (DSCP occupies the high 6
 * bits of the TOS/DS byte). AES67 media is AF41 (DSCP 34); call after open on
 * the media sockets so RTP is prioritised on a switched fabric. Returns 0/-1. */
int aes67_send_set_dscp(struct aes67_sender *s, int dscp);

/* Send one datagram (a complete RTP packet). Returns bytes sent or -1. */
long aes67_send_packet(struct aes67_sender *s, const uint8_t *buf, size_t len);

void aes67_send_close(struct aes67_sender *s);

#endif /* AES67_SEND_H */
