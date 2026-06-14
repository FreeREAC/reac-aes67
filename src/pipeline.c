// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "pipeline.h"
#include "reac_decode.h"

void pipeline_init_ex(struct pipeline *pl, const struct reac_mode *mode,
                      uint8_t payload_type, uint32_t ssrc,
                      int plc_xfade, int plc_fade_pkts)
{
	pl->mode = mode;
	media_clock_init(&pl->clock, mode->samples_per_pkt);
	pl->rtp.payload_type = payload_type;
	pl->rtp.seq = 0;
	pl->rtp.timestamp = 0;
	pl->rtp.ssrc = ssrc;
	plc_init(&pl->plc, mode, plc_fade_pkts, plc_xfade);
	pl->stats = (struct pipeline_stats){0};
	pl->stats.last_tier = -1;
	pl->pcm_sink = NULL;
	pl->pcm_ctx = NULL;
}

void pipeline_set_pcm_sink(struct pipeline *pl, pipeline_pcm_fn fn, void *ctx)
{
	pl->pcm_sink = fn;
	pl->pcm_ctx = ctx;
}

void pipeline_anchor(struct pipeline *pl, uint32_t anchor)
{
	/* re-init the media clock anchored; safe before the first frame */
	media_clock_init_anchored(&pl->clock, pl->mode->samples_per_pkt, anchor);
}

void pipeline_init(struct pipeline *pl, const struct reac_mode *mode,
                   uint8_t payload_type, uint32_t ssrc)
{
	pipeline_init_ex(pl, mode, payload_type, ssrc, 0, 0);
}

struct pipeline_stats pipeline_get_stats(const struct pipeline *pl)
{
	return pl->stats;
}

/* Build + emit one RTP packet from planar PCM at the given timestamp. */
static void emit_pcm(struct pipeline *pl, const uint8_t *planar, uint32_t ts,
                     int concealed, pipeline_emit_fn emit, void *ctx)
{
	if (pl->pcm_sink) {
		/* Dante path: hand planar PCM + PTP-anchored ts to the packetizer; it
		 * owns aggregation, flow-split, RTP build, and per-flow sequencing. */
		pl->pcm_sink(pl->pcm_ctx, planar, ts, concealed);
	} else {
		/* AES67 default: one RTP packet per REAC frame. */
		uint8_t pkt[RTP_HEADER_LEN + REAC_MAX_CHANNELS * 24 * 3];
		pl->rtp.timestamp = ts;
		long n = rtp_l24_build(pkt, sizeof pkt, &pl->rtp, planar, pl->mode);
		if (n > 0)
			emit(ctx, pkt, (size_t)n, concealed);
		pl->stats.last_seq = pl->rtp.seq;
		pl->rtp.seq++; /* every emitted packet consumes a sequence number */
	}
	if (concealed) pl->stats.concealed++;
	else pl->stats.packets++;
}

int pipeline_on_frame(struct pipeline *pl, const uint8_t *raw, size_t len,
                      pipeline_emit_fn emit, void *ctx)
{
	uint8_t pcm[REAC_MAX_CHANNELS * 24 * 3];
	int ns = reac_decode(raw, len, pl->mode, pcm);
	if (ns < 0) {
		pl->stats.malformed++;
		return -1; /* malformed: emit nothing */
	}

	struct reac_frame f = reac_frame_inspect(raw, len, pl->mode);
	struct media_clock_step step = media_clock_on_packet(&pl->clock, f.counter);
	pl->stats.loss_total += (uint64_t)step.silence_packets;

	/* Loss concealment: emit `silence_packets` fill packets BEFORE this one.
	 * Strategy = PLC (repeat-last + linear crossfade, burst gain-fade, hold-last
	 * floor, silence only at cold start).
	 * loss_index i selects the tier: 0 = first lost packet (crossfade seam),
	 * >=1 = burst body (decaying gain).
	 *
	 * step.rtp_ts is the timestamp for THIS (real) packet; the N concealment
	 * packets occupy the N preceding timestamps, spaced samples_per_pkt apart.
	 */
	int sps = pl->mode->samples_per_pkt;
	for (int i = 0; i < step.silence_packets; i++) {
		uint32_t cts = step.rtp_ts - (uint32_t)(step.silence_packets - i) * (uint32_t)sps;
		uint8_t cpcm[REAC_MAX_CHANNELS * 24 * 3];
		pl->stats.last_tier = plc_conceal(&pl->plc, cpcm, i);
		emit_pcm(pl, cpcm, cts, 1, emit, ctx);
	}

	/* the real packet */
	emit_pcm(pl, pcm, step.rtp_ts, 0, emit, ctx);

	/* update concealment history with this real packet */
	plc_observe(&pl->plc, pcm);
	return 0;
}
