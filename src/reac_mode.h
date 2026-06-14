// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* REAC stream mode descriptor.
 *
 * REAC (Roland audio-over-Ethernet, EtherType 0x8819) frames have a fixed
 * 1492-byte shape at the sample rates we care about:
 *
 *   50 B L2 header  (14 eth + uint16 LE l2_counter + uint16 l2_type + 32 unknown)
 * + 1440 B audio    (samples_per_pkt * n_channels * 3 bytes, interleaved)
 * +    2 B end marker 0xC2 0xEA
 * = 1492 B
 *
 * The frame is RATE-INVARIANT: always 12 samples x 40 channels x 3 B = 1440 B
 * audio, at every supported rate. The SAMPLE RATE is set by the PACKET RATE, not
 * by repacking the frame:
 *
 *   44.1 kHz : 12 samp x 40 ch, 44100/12 = 3675 pps   (272 us slot)
 *   48   kHz : 12 samp x 40 ch, 48000/12 = 4000 pps   (250 us slot; VERIFIED
 *              on-site + obs-h8819)
 *   96   kHz : 12 samp x 40 ch, 96000/12 = 8000 pps   (125 us slot; DOUBLE
 *              packet rate)
 *
 * 96 kHz model SETTLED (2026-06-06) — candidate (a) double-pps {96000, 40, 12}:
 * the on-rig 96k stream measured ~8000 pps carrying the same 1492 B / 40-ch
 * frame, matching per-gron/reacdriver REACConstants.h (SAMPLES_PER_PACKET=12,
 * PACKETS_PER_SECOND=8000, MAX_CHANNEL_COUNT=40). The rejected channel-halving
 * model {96000, 20, 24} would have been ~4000 pps / 20 ch; both the measured pps
 * and the channel count refuted it. (Payload-growth was already ruled out: the
 * 48k frame fills ~99% of the 1500 MTU and REAC is 100BASE-TX, so it adds
 * packets, never enlarges them. The R-1000 manual's "24 tracks @96k" that seeded
 * the halving model was the RECORDER's storage limit, not a wire constraint.)
 */
#ifndef REAC_MODE_H
#define REAC_MODE_H

#include <stdint.h>

#define REAC_ETHERTYPE 0x8819
#define REAC_L2_HEADER_LEN 50  /* 14 + 2 + 2 + 32 */
#define REAC_END_MARKER_0 0xC2
#define REAC_END_MARKER_1 0xEA
#define REAC_RESOLUTION 3      /* bytes per sample per channel (24-bit) */
#define REAC_AUDIO_BYTES 1440  /* samples_per_pkt * n_channels * RESOLUTION */
#define REAC_FRAME_BYTES (REAC_L2_HEADER_LEN + REAC_AUDIO_BYTES + 2)
#define REAC_MAX_CHANNELS 40   /* hard cap per REAC connection regardless of rate/stagebox */

struct reac_mode {
	int sample_rate;     /* 44100 / 48000 / 96000 */
	int n_channels;      /* 40 at every rate (frame is rate-invariant) */
	int samples_per_pkt; /* 12 per packet at every rate; rate = pps * 12 */
};

/* The verified 48 kHz mode. */
extern const struct reac_mode REAC_MODE_48K;

/* The 96 kHz mode, verified on-rig 2026-06-06: {96000, 40, 12} at ~8000 pps. */
extern const struct reac_mode REAC_MODE_96K;

#endif /* REAC_MODE_H */
