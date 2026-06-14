// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_rx — the non-realtime RX feeder.
 *
 * Owns the wire source (live AF_PACKET via reac_capture, or pcap replay via
 * pcap_source), validates + decodes each 0x8819 frame with the reac-aes67
 * plain-LE core, converts the planar s24 output to float, and pushes it into
 * the shared ring for the PipeWire process() callback.
 *
 * It is ALSO the rate authority: it tracks the byte-14/15 free-running counter
 * slope against CLOCK_MONOTONIC (or a NIC PHC), which gives both the recovered
 * REAC sample rate (via reac_rate_snap) and a slowly-filtered ppm error. The
 * source node reads that ppm and feeds it to PipeWire's io_rate_match so the
 * graph's async resampler tracks the desk's true rate (the Tier-A clock
 * bridge). This thread does NOT run SCHED_FIFO — it's the producer; only the
 * audio graph and (in Phase 2) the TX slot pacer are realtime.
 */
#ifndef REAC_RX_H
#define REAC_RX_H

#include <stdint.h>
#include <pthread.h>
#include "reac_ring.h"

enum reac_rx_kind {
	REAC_RX_PCAP,    /* offline replay (pcap_source) */
	REAC_RX_LIVE,    /* live AF_PACKET (reac_capture) */
};

struct reac_rx_cfg {
	enum reac_rx_kind kind;
	const char *source;   /* ifname for LIVE, file path for PCAP */
	int forced_rate;      /* 0 = auto-detect from cadence; else 44100/48000/96000 */
	int pcap_realtime;    /* PCAP only: pace replay by capture timestamps (1) vs flat out (0) */
};

struct reac_rx {
	struct reac_rx_cfg cfg;
	struct reac_ring *ring;       /* shared with the source node */
	int sample_rate;              /* recovered (snapped) REAC rate */

	pthread_t thread;
	_Atomic int running;

	/* rate-slope estimator (counter-vs-monotonic), filtered ppm error vs the
	 * nominal recovered rate; the source node reads this for io_rate_match. */
	_Atomic int ppm_error_milli;  /* ppm * 1000, signed; 0 until enough samples */

	/* diagnostics */
	_Atomic uint64_t frames_ok;
	_Atomic uint64_t frames_bad;
	_Atomic uint64_t counter_gaps; /* lost frames inferred from counter jumps */
};

/* Open the source, detect (or accept the forced) rate, and allocate the ring
 * sized for the rate. Does NOT start the thread. Returns 0 / -1.
 * On success rx->sample_rate and rx->ring are valid. */
int reac_rx_open(struct reac_rx *rx, const struct reac_rx_cfg *cfg, struct reac_ring *ring);

/* Spawn the feeder thread. Returns 0 / -1. */
int reac_rx_start(struct reac_rx *rx);

/* Signal stop and join. */
void reac_rx_stop(struct reac_rx *rx);

void reac_rx_close(struct reac_rx *rx);

#endif /* REAC_RX_H */
