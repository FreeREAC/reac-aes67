// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// Portions derived from obs-h8819-source, Copyright (C) 2022 Norihiro Kamae <norihiro@nagater.net> (GPL-3.0-or-later).

/* Pure REAC frame decode: raw 1492-byte frame -> interleaved 24-bit PCM.
 * No I/O. The de-interleave math is lifted from norihiro/obs-h8819-source
 * (convert_to_pcm24lep), generalized to the reac_mode descriptor.
 */
#ifndef REAC_DECODE_H
#define REAC_DECODE_H

#include <stdint.h>
#include <stddef.h>
#include "reac_mode.h"

struct reac_frame {
	uint16_t counter;   /* l2_counter, little-endian */
	int valid;          /* 1 if ethertype + length + end marker checked out */
};

/* Validate a raw REAC frame and extract its l2_counter.
 * raw/len is the full ethernet frame (starting at the dest MAC).
 * Returns a reac_frame with valid=1 only if it is a well-formed 0x8819 frame
 * of the expected length with the 0xC2 0xEA end marker. */
struct reac_frame reac_frame_inspect(const uint8_t *raw, size_t len,
                                     const struct reac_mode *mode);

/* Decode the audio region of a validated frame into planar 24-bit LE PCM.
 *
 * out must hold mode->n_channels * mode->samples_per_pkt samples; each sample
 * is written as 3 bytes (little-endian, signed 24-bit), grouped by channel
 * (channel 0's samples first, then channel 1's, ...). i.e. out is planar:
 *   out[(ch*samples_per_pkt + s)*3 + {0,1,2}]
 *
 * raw/len is the full ethernet frame; the audio region begins at
 * REAC_L2_HEADER_LEN. Returns the number of samples written per channel, or
 * -1 on a malformed frame. */
int reac_decode(const uint8_t *raw, size_t len, const struct reac_mode *mode,
                uint8_t *out);

#endif /* REAC_DECODE_H */
