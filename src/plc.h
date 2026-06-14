// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* plc.h — packet loss concealment for the REAC->AES67 bridge.
 *
 * Strategy:
 * repeat-last-packet + short LINEAR in-splice crossfade, with a graceful
 * fallback ladder. Search-free and constant-time per channel, so a whole-frame
 * loss (up to 3 streams x 40 ch in one 250us slot) cannot miss the egress
 * deadline — which is why pitch-similarity/WSOLA was rejected as primary.
 *
 * Operates on the project's planar signed-24-bit-LE layout: sample s of channel
 * ch is at pcm[(ch*S + s)*3 + {0,1,2}], LE signed, S = mode->samples_per_pkt
 * (12 @48k/40ch, 24 @96k/20ch).
 *
 * Fallback ladder (operator mandate: "keep the same sound if no better
 * pattern" — hold-last is the FLOOR, silence only as last resort):
 *   TIER 0 repeat + linear crossfade  (first lost packet of a run; needs >=1 full prior packet)
 *   TIER 1 scaled-repeat, decaying gain (burst body; fades toward silence)
 *   TIER 2 hold-last (DC of last sample) (history underfilled but >=1 sample)
 *   TIER 3 silence (zeros)              (cold start: loss before any audio)
 *
 * All integer (int32 history, Q15 ramps/gains). Per-channel, deterministic.
 */
#ifndef PLC_H
#define PLC_H

#include <stdint.h>
#include "reac_mode.h"

#define PLC_HIST_SAMPLES 40 /* >= S(max 24) + crossfade margin; static, per channel */

enum {
	PLC_TIER_REPEAT_XFADE = 0,
	PLC_TIER_BURST_FADE   = 1,
	PLC_TIER_HOLD_LAST    = 2,
	PLC_TIER_SILENCE      = 3
};

struct plc_chan {
	int32_t hist[PLC_HIST_SAMPLES]; /* ring of recent decoded samples */
	int head;                       /* next write index (newest at head-1) */
	int filled;                     /* valid samples in hist (0..PLC_HIST_SAMPLES) */
};

struct plc {
	int S;                  /* samples_per_pkt */
	int n_channels;
	int xfade_w;            /* in-splice crossfade length (samples, <= S) */
	int fade_packets;       /* burst fade length in packets -> silence */
	int fade_flat_packets;  /* packets at full gain before decay begins */
	struct plc_chan ch[REAC_MAX_CHANNELS];
};

/* Initialise. xfade_w<=0 → default min(S,8); fade_packets<=0 → default 200. */
void plc_init(struct plc *st, const struct reac_mode *mode,
              int fade_packets, int xfade_w);

/* GOOD-PACKET PATH: push one decoded planar S24LE packet into per-channel
 * history. Call once per real packet before/after emit. Cheap (S stores/ch). */
void plc_observe(struct plc *st, const uint8_t *pcm);

/* LOST-PACKET PATH: synthesize one concealment packet into out (planar S24LE,
 * capacity n_channels*S*3). loss_index = 0-based position within the current
 * consecutive-loss run (0 = first lost packet → crossfade; >=1 → burst fade).
 * Returns the worst (highest) tier used across channels. Constant-time. */
int plc_conceal(struct plc *st, uint8_t *out, int loss_index);

#endif /* PLC_H */
