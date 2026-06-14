// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_capture — live REAC frame capture via AF_PACKET.
 *
 * Opens a raw packet socket bound to an interface, optionally filtered to a
 * source MAC, and yields full ethernet frames. EtherType filtering for 0x8819
 * is done in-loop (a classic BPF could be attached later). Needs CAP_NET_RAW.
 *
 * The capture LOOP (frame -> pipeline) is identical to the pcap-replay path;
 * only the raw-socket open here is privileged/untestable in CI, so it is kept
 * minimal. Off-hardware, use pcap_source instead.
 */
#ifndef REAC_CAPTURE_H
#define REAC_CAPTURE_H

#include <stdint.h>
#include <stddef.h>

struct reac_capture {
	int fd;
};

/* Open a raw AF_PACKET socket bound to ifname. Returns 0 / -1. */
int reac_capture_open(struct reac_capture *c, const char *ifname);

/* Toggle non-blocking mode on the capture socket. In non-blocking mode
 * reac_capture_next returns 0 when no frame is currently available (EAGAIN),
 * which lets an event loop drain all ready frames then yield. Returns 0/-1. */
int reac_capture_set_nonblock(struct reac_capture *c, int nonblock);

/* Read the next ethernet frame into buf (cap). Returns length (>0), 0 if no
 * frame is available right now (non-blocking only), or -1 on error. Only frames
 * with EtherType 0x8819 are returned; others are skipped. In blocking mode this
 * waits for a frame; 0 is never returned. */
long reac_capture_next(struct reac_capture *c, uint8_t *buf, size_t cap);

void reac_capture_close(struct reac_capture *c);

#endif /* REAC_CAPTURE_H */
