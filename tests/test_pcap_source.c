// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Read the shared reac-tools fixture and confirm we get the 4 known frames,
 * each decodable, with the expected seq chain 0xfd7e..0xfd81. */
#define _POSIX_C_SOURCE 200809L  /* mkstemp / fdopen / unlink under -std=c11 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/pcap_source.h"
#include "../src/reac_decode.h"
#include <reac/reac.h>
#include "test_util.h"

static const char *fixture(void)
{
	const char *p = getenv("REAC_FIXTURE");
	return p ? p : "../reac-tools/tests/fixtures/real_reac_stream.pcap";
}

static int test_reads_four_frames_with_seq_chain(void)
{
	struct pcap_source ps;
	ASSERT_EQ_INT(pcap_source_open(&ps, fixture()), 0);

	uint8_t buf[2048];
	uint16_t expect[4] = { 0xfd7e, 0xfd7f, 0xfd80, 0xfd81 };
	int i = 0;
	long n;
	while ((n = pcap_source_next(&ps, buf, sizeof buf, NULL)) > 0) {
		ASSERT(i < 4);
		struct reac_frame f = reac_frame_inspect(buf, (size_t)n, &REAC_MODE_48K);
		ASSERT(f.valid);
		ASSERT_EQ_INT(f.counter, expect[i]);
		i++;
	}
	ASSERT_EQ_INT(i, 4);
	pcap_source_close(&ps);
	return 0;
}

static int test_open_missing_fails(void)
{
	struct pcap_source ps;
	ASSERT_EQ_INT(pcap_source_open(&ps, "/no/such/file.pcap"), -1);
	return 0;
}

static void wr_le32(FILE *f, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	fwrite(b, 1, 4, f);
}

/* one pcap record: 16-byte header (ts/incl/orig) + incl filler bytes */
static void wr_rec(FILE *f, uint32_t incl)
{
	wr_le32(f, 0);      /* ts_sec  */
	wr_le32(f, 0);      /* ts_usec */
	wr_le32(f, incl);   /* incl_len */
	wr_le32(f, incl);   /* orig_len */
	for (uint32_t i = 0; i < incl; i++)
		fputc((int)(i & 0xff), f);
}

/* An oversized frame (incl_len > the caller buffer) must be skipped, not treated
 * as a read error that silently aborts the rest of the capture. */
static int test_oversized_frame_is_skipped_not_aborting(void)
{
	char path[] = "/tmp/reac_oversized_XXXXXX";
	int fd = mkstemp(path);
	ASSERT(fd >= 0);
	FILE *f = fdopen(fd, "wb");
	ASSERT(f != NULL);
	/* classic little-endian pcap global header, linktype Ethernet */
	wr_le32(f, 0xA1B2C3D4u);
	uint8_t gh_rest[20] = {
		2, 0, 4, 0,        /* version 2.4   */
		0, 0, 0, 0,        /* thiszone      */
		0, 0, 0, 0,        /* sigfigs       */
		0, 0x40, 0, 0,     /* snaplen 16384 */
		1, 0, 0, 0,        /* network = LINKTYPE_ETHERNET */
	};
	fwrite(gh_rest, 1, sizeof gh_rest, f);
	wr_rec(f, 64);         /* frame A — fits */
	wr_rec(f, 4096);       /* oversized — exceeds the 2048-byte caller buffer */
	wr_rec(f, 64);         /* frame B — must STILL be read after the skip */
	fclose(f);

	struct pcap_source ps;
	ASSERT_EQ_INT(pcap_source_open(&ps, path), 0);
	uint8_t buf[2048];
	int n_frames = 0;
	long n;
	while ((n = pcap_source_next(&ps, buf, sizeof buf, NULL)) > 0) {
		ASSERT_EQ_INT(n, 64);
		n_frames++;
	}
	ASSERT_EQ_INT((int)n, 0);     /* clean EOF, not a -1 error */
	ASSERT_EQ_INT(n_frames, 2);   /* both fitting frames; oversized one skipped */
	pcap_source_close(&ps);
	unlink(path);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= RUN(test_reads_four_frames_with_seq_chain);
	rc |= RUN(test_open_missing_fails);
	rc |= RUN(test_oversized_frame_is_skipped_not_aborting);
	return rc;
}
