// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_tx.h"

#include <reac/reac.h>      /* REAC_FRAME_BYTES, _AUDIO_OFFSET, _HDR_COUNTER_OFF, ... */

#include <string.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>     /* htons */

/* normalized float [-1,1) -> 24-bit signed LE, the exact inverse of reac_rx.c's
 * s24le_to_f32 (which divides by 2^23), so an encode->decode round-trip is the
 * identity up to one ULP of 24-bit quantization. */
static inline void f32_to_s24le(float v, uint8_t *p)
{
	float x = v * 8388608.0f;            /* 2^23 */
	if (x > 8388607.0f) x = 8388607.0f;  /* clamp to the 24-bit signed range */
	if (x < -8388608.0f) x = -8388608.0f;
	int32_t s = (int32_t)lrintf(x);
	p[0] = (uint8_t)(s & 0xFF);
	p[1] = (uint8_t)((s >> 8) & 0xFF);
	p[2] = (uint8_t)((s >> 16) & 0xFF);
}

int reac_tx_build(uint8_t *out, float *const *planar, int nch, int ns,
                  uint16_t counter, const uint8_t src[6])
{
	memset(out, 0, REAC_FRAME_BYTES);

	/* L2: broadcast dst (downstream master), our src, EtherType 0x8819. */
	memset(out, 0xFF, 6);
	memcpy(out + 6, src, 6);
	out[12] = (REAC_ETHERTYPE >> 8) & 0xFF;   /* 0x88 */
	out[13] = REAC_ETHERTYPE & 0xFF;          /* 0x19 */

	/* u16-LE sequence counter at byte 14; type 0x0000 (FILLER carries audio and
	 * is checksum-exempt) + the 32-byte control block at 18..49 stay zero. */
	out[REAC_HDR_COUNTER_OFF]     = (uint8_t)(counter & 0xFF);
	out[REAC_HDR_COUNTER_OFF + 1] = (uint8_t)((counter >> 8) & 0xFF);

	/* Audio: plain-LE, sample-major. For each time-sample s all 40 channels in
	 * order, so channel ch / sample s lands at (s*40 + ch)*3 — the exact slot
	 * reac_decode reads back. Channels beyond nch (or NULL planes) are silent. */
	uint8_t *audio = out + REAC_AUDIO_OFFSET;
	const int N = REAC_MAX_CHANNELS;
	int frames = ns < REAC_SAMPLES_PER_PKT ? ns : REAC_SAMPLES_PER_PKT;
	for (int s = 0; s < frames; s++) {
		for (int ch = 0; ch < N; ch++) {
			float v = (ch < nch && planar[ch]) ? planar[ch][s] : 0.0f;
			f32_to_s24le(v, audio + (size_t)(s * N + ch) * REAC_RESOLUTION);
		}
	}

	out[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;  /* 0xC2 */
	out[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;  /* 0xEA */
	return REAC_FRAME_BYTES;
}

int reac_tx_open(struct reac_tx *tx, const char *ifname)
{
	tx->fd = -1;
	tx->ifindex = 0;
	tx->counter = 0;
	/* Stand-in source MAC: Roland OUI 00:40:ab + a fixed host part. The decoder
	 * ignores src, and this keeps emitted frames sanitization-clean (no rig MAC). */
	static const uint8_t standin[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 };
	memcpy(tx->src, standin, 6);

	int fd = socket(AF_PACKET, SOCK_RAW, htons(REAC_ETHERTYPE));
	if (fd < 0)
		return -1;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		close(fd);
		return -1;
	}
	tx->ifindex = ifr.ifr_ifindex;
	tx->fd = fd;
	return 0;
}

void reac_tx_close(struct reac_tx *tx)
{
	if (tx->fd >= 0)
		close(tx->fd);
	tx->fd = -1;
}

int reac_tx_emit(struct reac_tx *tx, float *const *planar, int nch, int ns)
{
	uint8_t frame[REAC_FRAME_BYTES];
	reac_tx_build(frame, planar, nch, ns, tx->counter, tx->src);

	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family  = AF_PACKET;
	sll.sll_ifindex = tx->ifindex;
	sll.sll_halen   = 6;
	memset(sll.sll_addr, 0xFF, 6);  /* broadcast dst */

	ssize_t r = sendto(tx->fd, frame, REAC_FRAME_BYTES, 0,
	                   (struct sockaddr *)&sll, sizeof sll);
	tx->counter++;  /* free-running, wraps at 16 bits like the desk's */
	return (int)r;
}
