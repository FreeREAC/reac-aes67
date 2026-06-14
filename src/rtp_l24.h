// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* RTP L24 (24-bit linear PCM) packetization for AES67.
 *
 * Pure, socket-free: builds the bytes of one RTP packet from a decoded REAC
 * packet's planar PCM. The UDP/multicast send is a separate concern
 * (aes67_send), so the wire format is fully unit-testable off-hardware.
 *
 * AES67 L24 wire format:
 *   12-byte RTP header (RFC 3550), then audio as INTERLEAVED big-endian 24-bit
 *   signed samples, frame-major: [s0c0 s0c1 ... s0c{N-1}] [s1c0 ...] ...
 *
 * Our decoded input is PLANAR little-endian 24-bit (channel-major), as
 * produced by reac_decode. So packetization both RE-INTERLEAVES (planar ->
 * frame-major) and FLIPS endianness (LE -> BE).
 */
#ifndef RTP_L24_H
#define RTP_L24_H

#include <stdint.h>
#include <stddef.h>
#include "reac_mode.h"

#define RTP_HEADER_LEN 12
#define RTP_VERSION 2

struct rtp_params {
	uint8_t payload_type; /* dynamic PT, 96..127 */
	uint16_t seq;         /* RTP sequence number (per-packet, monotonic) */
	uint32_t timestamp;   /* media clock sample count */
	uint32_t ssrc;        /* synchronization source id */
};

/* Build one RTP L24 packet into out (capacity cap).
 * planar_pcm holds mode->n_channels * mode->samples_per_pkt planar LE 24-bit
 * samples (channel-major), i.e. reac_decode output.
 * Returns total packet length (header + payload), or -1 if cap too small. */
long rtp_l24_build(uint8_t *out, size_t cap,
                   const struct rtp_params *p,
                   const uint8_t *planar_pcm,
                   const struct reac_mode *mode);

/* Build one RTP L24 packet from a CHANNEL SLICE of a planar block, for Dante
 * AES67 flow splitting (40 ch -> several <=8-ch flows). planar_pcm holds
 * total_nch channels, n_samples each (channel-major, LE 24-bit). Only channels
 * [ch_start, ch_start+flow_nch) are emitted, re-interleaved frame-major BE.
 * n_samples may exceed one REAC frame (e.g. 48 for a 1 ms Dante packet).
 * Returns total packet length, or -1 if cap too small / slice out of range.
 * rtp_l24_build() is the (0, all-channels, samples_per_pkt) special case. */
long rtp_l24_build_flow(uint8_t *out, size_t cap,
                        const struct rtp_params *p,
                        const uint8_t *planar_pcm,
                        int total_nch, int ch_start, int flow_nch,
                        int n_samples);

#endif /* RTP_L24_H */
