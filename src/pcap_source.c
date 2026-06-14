// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "pcap_source.h"
#include <stdio.h>
#include <string.h>

#define PCAP_MAGIC      0xA1B2C3D4u
#define PCAP_MAGIC_SWAP 0xD4C3B2A1u

static uint32_t rd_u32(const uint8_t *p, int swap)
{
	uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	             ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	if (swap)
		v = __builtin_bswap32(v);
	return v;
}

int pcap_source_open(struct pcap_source *ps, const char *path)
{
	ps->f = fopen(path, "rb");
	if (!ps->f)
		return -1;
	uint8_t gh[24];
	if (fread(gh, 1, 24, ps->f) != 24) {
		fclose(ps->f);
		ps->f = NULL;
		return -1;
	}
	uint32_t magic = (uint32_t)gh[0] | ((uint32_t)gh[1] << 8) |
	                 ((uint32_t)gh[2] << 16) | ((uint32_t)gh[3] << 24);
	if (magic == PCAP_MAGIC)
		ps->swapped = 0;
	else if (magic == PCAP_MAGIC_SWAP)
		ps->swapped = 1;
	else {
		fclose(ps->f);
		ps->f = NULL;
		return -1;
	}
	return 0;
}

long pcap_source_next(struct pcap_source *ps, uint8_t *buf, size_t cap, uint64_t *ts_usec)
{
	if (!ps->f)
		return -1;
	for (;;) {
		uint8_t ph[16];
		size_t got = fread(ph, 1, 16, ps->f);
		if (got == 0)
			return 0; /* EOF */
		if (got != 16)
			return -1;
		uint32_t sec = rd_u32(ph, ps->swapped);
		uint32_t usec = rd_u32(ph + 4, ps->swapped);
		uint32_t incl = rd_u32(ph + 8, ps->swapped);
		if (incl > cap) {
			/* Frame larger than the caller buffer (a jumbo/non-REAC packet, or
			 * a corrupt header). Skip its body and keep going — returning -1
			 * here is indistinguishable from a read error and would silently
			 * abort the rest of a multi-thousand-frame capture. */
			fprintf(stderr, "pcap_source: skipping oversized frame (%u > %zu bytes)\n",
			        incl, cap);
			if (fseek(ps->f, (long)incl, SEEK_CUR) != 0)
				return -1;
			continue;
		}
		if (fread(buf, 1, incl, ps->f) != incl)
			return -1;
		if (ts_usec)
			*ts_usec = (uint64_t)sec * 1000000ull + usec;
		return (long)incl;
	}
}

void pcap_source_close(struct pcap_source *ps)
{
	if (ps->f) {
		fclose(ps->f);
		ps->f = NULL;
	}
}
