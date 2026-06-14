// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "packetizer.h"
#include "rtp_l24.h"
#include <string.h>

int dante_build_flows(struct reac_flow *flows, int max, int n_channels,
                      int ch_per_flow, uint8_t payload_type, uint32_t ssrc_base)
{
	if (ch_per_flow <= 0 || n_channels <= 0)
		return -1;
	int n = (n_channels + ch_per_flow - 1) / ch_per_flow;
	if (n > max)
		return -1;
	for (int i = 0; i < n; i++) {
		int start = i * ch_per_flow;
		int width = n_channels - start;
		if (width > ch_per_flow)
			width = ch_per_flow;
		flows[i].ch_start = start;
		flows[i].n_ch = width;
		flows[i].payload_type = payload_type;
		flows[i].ssrc = ssrc_base + (uint32_t)i;
		flows[i].seq = 0;
	}
	return n;
}

void packetizer_init(struct packetizer *pk, const struct reac_mode *mode,
                     int frames_per_packet, struct reac_flow *flows, int n_flows)
{
	int fpp = frames_per_packet;
	if (fpp < 1)
		fpp = 1;
	if (fpp > PACKETIZER_MAX_FPP)
		fpp = PACKETIZER_MAX_FPP;
	pk->mode = mode;
	pk->frames_per_packet = fpp;
	pk->frames_buffered = 0;
	pk->pending_ts = 0;
	pk->pending_concealed = 0;
	pk->flows = flows;
	pk->n_flows = n_flows;
}

void packetizer_push(struct packetizer *pk, const uint8_t *frame_planar,
                     uint32_t rtp_ts, int concealed,
                     packetizer_emit_fn emit, void *ctx)
{
	const int nch = pk->mode->n_channels;
	const int sps = pk->mode->samples_per_pkt;
	const int total_samples = pk->frames_per_packet * sps;
	const int f = pk->frames_buffered;

	if (f == 0) {
		pk->pending_ts = rtp_ts;   /* the packet carries the FIRST frame's ts */
		pk->pending_concealed = 0;
	}
	pk->pending_concealed |= concealed ? 1 : 0;

	/* Append this frame to the planar window: channel c's sps samples are
	 * contiguous in the frame; place them at sample offset f*sps in the big
	 * per-channel region [c*total_samples, c*total_samples+total_samples). */
	for (int c = 0; c < nch; c++) {
		uint8_t *dst = pk->planar + (size_t)(c * total_samples + f * sps) * 3;
		const uint8_t *src = frame_planar + (size_t)(c * sps) * 3;
		memcpy(dst, src, (size_t)sps * 3);
	}
	pk->frames_buffered++;

	if (pk->frames_buffered < pk->frames_per_packet)
		return;

	/* window full: one RTP packet per flow (its channel slice), then reset. */
	for (int i = 0; i < pk->n_flows; i++) {
		struct reac_flow *fl = &pk->flows[i];
		struct rtp_params p = {
			.payload_type = fl->payload_type,
			.seq = fl->seq,
			.timestamp = pk->pending_ts,
			.ssrc = fl->ssrc,
		};
		uint8_t pkt[RTP_HEADER_LEN +
		            REAC_MAX_CHANNELS * PACKETIZER_MAX_FPP * 24 * 3];
		long n = rtp_l24_build_flow(pkt, sizeof pkt, &p, pk->planar,
		                            nch, fl->ch_start, fl->n_ch, total_samples);
		if (n > 0) {
			emit(ctx, i, pkt, (size_t)n, pk->pending_concealed);
			fl->seq++;
		}
	}
	pk->frames_buffered = 0;
}
