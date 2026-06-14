// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_tx — REAC downstream-frame encoder + raw-socket emitter (the inverse of
 * the reac-aes67 decode core). Builds a 1492-byte FILLER frame: broadcast dst,
 * our OUI src, EtherType 0x8819, u16-LE counter, type 0x0000 (FILLER carries
 * audio and is checksum-exempt), a zero 32-byte control block, 1440 B audio
 * (40 ch x 12 samples x 3 B, plain-LE sample-major (s*40+ch)*3), 0xC2 0xEA tail.
 *
 * First cut for the loopback demo (Rhythmbox -> reac:playback -> wire ->
 * reac:capture). Emits a downstream master broadcast; it does NOT yet drive the
 * connection handshake, so a real Roland desk will not link to it — that is the
 * separate JOIN/HOLD + master-role work. */
#ifndef REAC_TX_H
#define REAC_TX_H

#include <stdint.h>
#include <stddef.h>

struct reac_tx {
	int fd;            /* AF_PACKET socket, -1 if not open */
	int ifindex;
	uint8_t src[6];    /* our source MAC (Roland OUI + stand-in) */
	uint16_t counter;  /* free-running u16, +1 per emitted frame */
};

/* Build one downstream REAC frame into out[REAC_FRAME_BYTES] from planar float
 * input planar[ch][s] (ns samples/channel, nch channels mapped onto the 40-ch
 * frame; the rest are silent). `counter` is stamped at bytes 14-15 LE; `src` is
 * the 6-byte source MAC. Returns REAC_FRAME_BYTES. No socket needed — pure +
 * unit-testable (round-trips through reac_decode). */
int reac_tx_build(uint8_t *out, float *const *planar, int nch, int ns,
                  uint16_t counter, const uint8_t src[6]);

/* Open an AF_PACKET raw TX socket on `ifname` (needs CAP_NET_RAW). 0 / -1. */
int reac_tx_open(struct reac_tx *tx, const char *ifname);
void reac_tx_close(struct reac_tx *tx);

/* Build + emit one frame (ns samples x nch) on the wire; counter auto-increments.
 * Returns bytes sent, or -1. */
int reac_tx_emit(struct reac_tx *tx, float *const *planar, int nch, int ns);

#endif /* REAC_TX_H */
