// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Per-stream media clock for the AES67 sender.
 *
 * The RTP timestamp of an AES67 L24 stream is a running SAMPLE COUNT, not a
 * wall clock. We increment it by samples_per_pkt for every REAC packet. When
 * the REAC l2_counter shows a gap of N packets (loss), we advance the count by
 * N*samples_per_pkt as well, so the timeline and all channels stay aligned;
 * the caller emits N*samples_per_pkt of silence to fill the hole. This keeps a
 * lost packet from slipping every subsequent sample (which would shift every
 * channel) — see the REAC clicking investigation.
 */
#ifndef MEDIA_CLOCK_H
#define MEDIA_CLOCK_H

#include <stdint.h>
#include <stdbool.h>

struct media_clock {
	uint32_t rtp_ts;       /* current RTP timestamp (sample count, wraps at 2^32) */
	uint32_t anchor;       /* RTP ts of the first packet (0 = free-running AES67;
	                        * a TAI-derived value = PTP-locked / RFC 7273 Dante) */
	uint16_t last_counter; /* last REAC l2_counter seen */
	bool started;          /* false until the first packet */
	int samples_per_pkt;   /* 12 at every rate (frame rate-invariant; rate = pps x 12) */
	uint64_t total_lost;   /* cumulative lost packets (diagnostics) */
};

/* Free-running clock (AES67 default): the first packet's timestamp is 0. */
void media_clock_init(struct media_clock *mc, int samples_per_pkt);

/* PTP-locked clock (Dante / RFC 7273 mediaclk:direct=0): the first packet's
 * timestamp is `anchor` (the TAI-derived sample count at stream start), and the
 * timeline advances from there. Under genlock, sample-counting stays TAI-aligned
 * forever. See ptp_clock.h / the Dante design doc. */
void media_clock_init_anchored(struct media_clock *mc, int samples_per_pkt,
                               uint32_t anchor);

/* Result of accounting one received REAC packet. */
struct media_clock_step {
	uint32_t rtp_ts;       /* RTP timestamp to stamp THIS packet's samples with */
	int silence_packets;   /* packets to synthesize as silence BEFORE this one (loss fill) */
};

/* Account for a received packet with the given l2_counter.
 * Returns the RTP timestamp for this packet's audio and how many lost packets
 * preceded it (to be emitted as silence). */
struct media_clock_step media_clock_on_packet(struct media_clock *mc, uint16_t counter);

#endif /* MEDIA_CLOCK_H */
