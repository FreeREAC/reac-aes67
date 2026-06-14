// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* tone — synthetic multichannel test-tone generator (for the streaming demo).
 *
 * Fills one packet of planar signed-24-bit-LE PCM (the same layout reac_decode
 * produces and rtp_l24 consumes) with a continuous sine, so a running daemon
 * can emit a real, audible 40-channel AES67 stream without the REAC hardware.
 * Pure + deterministic given the phase state; no I/O.
 */
#ifndef TONE_H
#define TONE_H

#include <stdint.h>
#include <reac/reac.h>

struct tone {
	double phase;     /* running phase, radians, advanced across packets */
	int freq_hz;      /* tone frequency */
	int amplitude;    /* peak amplitude in 24-bit counts (<= 8388607) */
	int per_channel_detune; /* Hz added per channel so each is distinguishable */
};

void tone_init(struct tone *t, int freq_hz, int amplitude, int per_channel_detune);

/* Fill out (n_channels * samples_per_pkt planar 24-bit LE samples) with the
 * next packet of tone and advance the phase. Each channel ch gets
 * (freq_hz + ch*per_channel_detune). */
void tone_fill(struct tone *t, uint8_t *out, const struct reac_mode *mode);

#endif /* TONE_H */
