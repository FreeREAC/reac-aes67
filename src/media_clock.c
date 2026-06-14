// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "media_clock.h"

void media_clock_init_anchored(struct media_clock *mc, int samples_per_pkt,
                               uint32_t anchor)
{
	mc->rtp_ts = anchor;
	mc->anchor = anchor;
	mc->last_counter = 0;
	mc->started = false;
	mc->samples_per_pkt = samples_per_pkt;
	mc->total_lost = 0;
}

void media_clock_init(struct media_clock *mc, int samples_per_pkt)
{
	media_clock_init_anchored(mc, samples_per_pkt, 0);
}

struct media_clock_step media_clock_on_packet(struct media_clock *mc, uint16_t counter)
{
	struct media_clock_step step = { 0, 0 };

	if (!mc->started) {
		mc->started = true;
		mc->last_counter = counter;
		mc->rtp_ts = mc->anchor;
		step.rtp_ts = mc->anchor;
		step.silence_packets = 0;
		return step;
	}

	/* Signed 16-bit delta vs the expected next counter (last+1): consecutive = 0,
	 * a forward gap = small positive (= lost), a duplicate or reordered (late)
	 * packet = negative. Interpreting the raw UNSIGNED value as "lost" would
	 * synthesize up to 65535 concealment packets for a single dup/reorder — so
	 * read it signed (same convention as reac-tools analyzer._signed_delta). */
	int16_t delta = (int16_t)(uint16_t)(counter - mc->last_counter - 1);

	if (delta < 0) {
		/* duplicate or reordered packet: not a loss. Emit it at the next
		 * timeline slot, synthesize no concealment, and keep last_counter as the
		 * high-water mark (don't rewind, or the next packet would look lost). */
		mc->rtp_ts += (uint32_t)mc->samples_per_pkt;
		step.rtp_ts = mc->rtp_ts;
		step.silence_packets = 0;
		return step;
	}

	uint16_t lost = (uint16_t)delta;
	step.silence_packets = lost;
	mc->total_lost += lost;

	/* advance timeline over the lost (silence) packets, then to this packet */
	mc->rtp_ts += (uint32_t)(lost + 1) * (uint32_t)mc->samples_per_pkt;
	step.rtp_ts = mc->rtp_ts;

	mc->last_counter = counter;
	return step;
}
