// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Read REAC frames from a classic pcap file (off-site dev / test source).
 *
 * Pairs with reac-tools' fixture writer. Classic libpcap only (magic
 * 0xA1B2C3D4 / swapped), link-type 1 (Ethernet). Yields full ethernet frames
 * so the same reac_decode path runs as for live AF_PACKET capture.
 */
#ifndef PCAP_SOURCE_H
#define PCAP_SOURCE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

struct pcap_source {
	FILE *f;
	int swapped;   /* byte-swap per-packet headers */
};

/* Open a pcap file. Returns 0 on success, -1 on error. */
int pcap_source_open(struct pcap_source *ps, const char *path);

/* Read the next packet into buf (capacity cap). Returns the packet length,
 * 0 at EOF, or -1 on error. ts_usec, if non-NULL, gets the capture time in
 * microseconds since epoch. */
long pcap_source_next(struct pcap_source *ps, uint8_t *buf, size_t cap, uint64_t *ts_usec);

void pcap_source_close(struct pcap_source *ps);

#endif /* PCAP_SOURCE_H */
