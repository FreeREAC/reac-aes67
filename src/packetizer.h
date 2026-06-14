// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* packetizer — aggregate decoded REAC frames into AES67/Dante RTP packets.
 *
 * REAC delivers 12 samples (250 us) per frame. Dante's AES67 interop wants
 * 1 ms packets (48 samples = 4 REAC frames) split into <=8-channel multicast
 * flows. The packetizer sits between the pipeline (which produces one decoded
 * planar PCM frame at a time, real or concealed, with a PTP-anchored RTP
 * timestamp) and the sender: it accumulates `frames_per_packet` frames into a
 * contiguous planar block, then builds ONE RTP L24 packet per configured flow
 * (its channel slice) and emits them.
 *
 * frames_per_packet = 1 reproduces the per-frame AES67 behaviour (250 us);
 * frames_per_packet = 4 is the Dante 1 ms profile. Socket-free + deterministic,
 * so the aggregation and flow-split are fully unit-testable.
 */
#ifndef PACKETIZER_H
#define PACKETIZER_H

#include <stdint.h>
#include <stddef.h>
#include "reac_mode.h"

#define PACKETIZER_MAX_FPP 4   /* Dante 1 ms @ 48 kHz = 4 REAC frames */

/* One AES67/Dante multicast flow: a contiguous channel slice with its own RTP
 * identity. The destination address/port live in the sender layer (keyed by the
 * flow index), so the packetizer stays socket-free. */
struct reac_flow {
	int ch_start;          /* first channel (0-based) */
	int n_ch;              /* channels in this flow (<=8 for Dante) */
	uint8_t payload_type;  /* dynamic RTP PT */
	uint32_t ssrc;         /* per-flow synchronization source */
	uint16_t seq;          /* per-flow RTP sequence; advanced by the packetizer */
};

/* Emitted once per flow per aggregated packet. buf/len is a complete RTP L24
 * packet; concealed flags a packet whose audio includes synthesized loss-fill. */
typedef void (*packetizer_emit_fn)(void *ctx, int flow_index,
                                   const uint8_t *buf, size_t len, int concealed);

struct packetizer {
	const struct reac_mode *mode;
	int frames_per_packet;
	int frames_buffered;
	uint32_t pending_ts;       /* RTP ts of the FIRST frame in the window */
	int pending_concealed;     /* set if any buffered frame was concealed */
	struct reac_flow *flows;   /* caller-owned; seq is mutated in place */
	int n_flows;
	/* accumulation: n_channels x (FPP*samples_per_pkt) planar LE 24-bit,
	 * sized for the worst case (40 ch x 4 frames x 24 samp x 3 B). */
	uint8_t planar[REAC_MAX_CHANNELS * PACKETIZER_MAX_FPP * 24 * 3];
};

/* frames_per_packet is clamped to [1, PACKETIZER_MAX_FPP]. flows is caller-owned
 * and must outlive the packetizer. */
void packetizer_init(struct packetizer *pk, const struct reac_mode *mode,
                     int frames_per_packet, struct reac_flow *flows, int n_flows);

/* Push one decoded per-frame planar block (mode->n_channels x
 * mode->samples_per_pkt, planar LE 24-bit). When frames_per_packet frames have
 * accumulated, builds + emits one RTP packet per flow and resets. */
void packetizer_push(struct packetizer *pk, const uint8_t *frame_planar,
                     uint32_t rtp_ts, int concealed,
                     packetizer_emit_fn emit, void *ctx);

/* Split [0, n_channels) into ch_per_flow-wide flows (the last may be narrower),
 * writing up to `max` into flows[]. Flow i gets ch_start=i*ch_per_flow, the
 * given payload_type, ssrc = ssrc_base + i, and seq = 0. Returns the flow count,
 * or -1 if more than `max` flows would be needed. */
int dante_build_flows(struct reac_flow *flows, int max, int n_channels,
                      int ch_per_flow, uint8_t payload_type, uint32_t ssrc_base);

#endif /* PACKETIZER_H */
