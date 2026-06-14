// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* End-to-end REAC->RTP pipeline (socket-free core).
 *
 * Ties together: reac_decode -> media_clock -> rtp_l24_build, plus loss
 * concealment for gaps. This is the testable heart of the daemon; reac_capture
 * feeds it frames (from AF_PACKET or pcap) and aes67_send transmits the RTP
 * packets it produces.
 *
 * Concealment: when media_clock reports N lost packets before a received one,
 * the pipeline emits N concealment packets FIRST (each its own RTP packet with
 * its own timestamp), then the real packet. The concealment strategy is
 * pluggable; the default is hold-last (repeat the previous packet's PCM) —
 * never silence — per operator preference. (A better PLC may replace the
 * default later; the seam is plc_conceal.)
 */
#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdint.h>
#include <stddef.h>
#include <reac/reac.h>
#include "media_clock.h"
#include "rtp_l24.h"
#include "plc.h"

/* Live counters for the ubus status object. Pure counts only — time-derived
 * values (packets/sec, uptime) are computed by the ubus layer from a monotonic
 * clock so this struct stays deterministic and unit-testable. */
struct pipeline_stats {
	uint64_t packets;      /* real (non-concealed) RTP packets emitted */
	uint64_t concealed;    /* concealment packets emitted */
	uint64_t loss_total;   /* lost REAC packets detected (seq gaps) */
	uint64_t malformed;    /* frames rejected by decode */
	uint16_t last_seq;     /* last RTP sequence number emitted */
	int last_tier;         /* last PLC tier used on a loss, or -1 if none yet */
};

/* Optional planar-PCM sink. When set (Dante profile), the pipeline hands each
 * decoded frame's planar PCM + PTP-anchored RTP timestamp to the sink (the
 * packetizer) INSTEAD of building one RTP packet per frame. */
typedef void (*pipeline_pcm_fn)(void *ctx, const uint8_t *planar,
                                uint32_t rtp_ts, int concealed);

struct pipeline {
	const struct reac_mode *mode;
	struct media_clock clock;
	struct rtp_params rtp;       /* seq/ssrc/pt persist; ts set per packet */
	struct plc plc;              /* loss concealment (repeat+xfade -> hold -> silence) */
	struct pipeline_stats stats;
	pipeline_pcm_fn pcm_sink;    /* NULL = build RTP per frame (AES67 default) */
	void *pcm_ctx;
};

/* Snapshot the current counters (cheap copy). */
struct pipeline_stats pipeline_get_stats(const struct pipeline *pl);

/* Full init: plc_xfade / plc_fade_pkts <= 0 use PLC defaults. */
void pipeline_init_ex(struct pipeline *pl, const struct reac_mode *mode,
                      uint8_t payload_type, uint32_t ssrc,
                      int plc_xfade, int plc_fade_pkts);

/* Convenience: default PLC tuning. */
void pipeline_init(struct pipeline *pl, const struct reac_mode *mode,
                   uint8_t payload_type, uint32_t ssrc);

/* Route decoded planar PCM to `fn` (the packetizer) instead of building one RTP
 * packet per frame. Pass fn=NULL to restore the default AES67 RTP path. */
void pipeline_set_pcm_sink(struct pipeline *pl, pipeline_pcm_fn fn, void *ctx);

/* PTP-lock the media clock: the first packet's RTP timestamp becomes `anchor`
 * (the TAI-derived sample count) instead of 0. Call after pipeline_init, before
 * the first frame. See ptp_clock.h / the Dante design. */
void pipeline_anchor(struct pipeline *pl, uint32_t anchor);

/* Callback invoked for each RTP packet the pipeline produces (real or
 * concealed). buf/len is a complete RTP L24 packet. is_concealed flags
 * synthesized loss-fill packets. */
typedef void (*pipeline_emit_fn)(void *ctx, const uint8_t *buf, size_t len,
                                 int is_concealed);

/* Feed one raw REAC ethernet frame. Validates+decodes, accounts the media
 * clock, emits any concealment packets for preceding loss, then emits this
 * packet. Returns 0 on success, -1 if the frame was malformed (nothing
 * emitted). */
int pipeline_on_frame(struct pipeline *pl, const uint8_t *raw, size_t len,
                      pipeline_emit_fn emit, void *ctx);

#endif /* PIPELINE_H */
