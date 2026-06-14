// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  /* clock_nanosleep / TIMER_ABSTIME */
#endif
#include "reac_rx.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <reac/reac.h>
/* the reac-aes67 plain-LE decode core + the two wire sources, reused as-is */
#include "reac_decode.h"
#include "reac_capture.h"
#include "pcap_source.h"

/* s24 LE (3 bytes) -> normalized float in [-1, 1) */
static inline float s24le_to_f32(const uint8_t *p)
{
	int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
	if (v & 0x00800000)
		v |= ~0x00FFFFFF; /* sign-extend bit 23 */
	return (float)v / 8388608.0f; /* 2^23 */
}

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Decode one validated frame into the ring (planar float, 40 ch x 12 samp). */
static void feed_frame(struct reac_rx *rx, const struct reac_mode *mode,
                       const uint8_t *frame, size_t len)
{
	uint8_t s24[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns = reac_decode(frame, len, mode, s24); /* planar s24: out[(ch*ns + s)*3] */
	if (ns < 0) {
		atomic_fetch_add_explicit(&rx->frames_bad, 1, memory_order_relaxed);
		return;
	}
	const int nch = mode->n_channels;
	float planar[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT];
	for (int ch = 0; ch < nch; ch++)
		for (int s = 0; s < ns; s++)
			planar[ch * ns + s] = s24le_to_f32(&s24[(size_t)(ch * ns + s) * 3]);
	reac_ring_write(rx->ring, planar, (uint32_t)ns);
	atomic_fetch_add_explicit(&rx->frames_ok, 1, memory_order_relaxed);
}

/* Slow PI-style slope filter: nominal pps vs observed counter advance per
 * second. Updates ppm_error_milli a few times a second. The source node turns
 * that into io_rate_match.rate. Kept deliberately gentle — locked to the
 * long-term counter slope, not instantaneous packet jitter (the design's
 * Tier-A loop). */
static void update_ppm(struct reac_rx *rx, uint16_t counter, uint64_t now_ns)
{
	static uint64_t win_start_ns;
	static uint32_t win_frames;
	static uint16_t last_counter;
	static int have_last;

	if (have_last) {
		win_frames += reac_counter_gap(last_counter, counter) + 1;
	} else {
		win_start_ns = now_ns;
		have_last = 1;
	}
	last_counter = counter;

	uint64_t dt = now_ns - win_start_ns;
	if (dt >= 250000000ull) { /* recompute ~4x/s */
		double obs_pps = (double)win_frames * 1e9 / (double)dt;
		double nom_pps = (double)rx->sample_rate / REAC_SAMPLES_PER_PKT;
		double ppm = (obs_pps - nom_pps) / nom_pps * 1e6;
		atomic_store_explicit(&rx->ppm_error_milli, (int)(ppm * 1000.0), memory_order_relaxed);
		win_start_ns = now_ns;
		win_frames = 0;
	}
}

static void *rx_loop(void *arg)
{
	struct reac_rx *rx = arg;
	const struct reac_mode *mode = reac_mode_for(rx->sample_rate);
	uint8_t frame[REAC_FRAME_BYTES + 64];

	struct reac_capture cap = { .fd = -1 };
	struct pcap_source ps = { 0 };
	int live = (rx->cfg.kind == REAC_RX_LIVE);

	/* Open the wire source inside the thread so the fd/FILE* lives and dies with
	 * it. (rate-detect in reac_rx_open used a SEPARATE short-lived capture.) */
	if (live) {
		if (reac_capture_open(&cap, rx->cfg.source) != 0)
			return NULL;
		reac_capture_set_nonblock(&cap, 0); /* blocking; EINTR/stop-flag exits */
	} else {
		if (pcap_source_open(&ps, rx->cfg.source) != 0)
			return NULL;
	}

	uint16_t last_counter = 0;
	int have_counter = 0;
	uint64_t pcap_first_ts = 0, wall_first_ns = 0;

	while (atomic_load_explicit(&rx->running, memory_order_acquire)) {
		long n;
		uint64_t pcap_ts = 0;
		if (live) {
			n = reac_capture_next(&cap, frame, sizeof frame);
		} else {
			n = pcap_source_next(&ps, frame, sizeof frame, &pcap_ts);
			if (n == 0) { /* EOF: loop the fixture for a steady offline source.
			               * Reset the pacing baseline (wall_first_ns) so the new
			               * loop re-paces from its first timestamp — otherwise
			               * every loop after the first replays FLAT OUT (targets
			               * land in the past), flooding the ring. */
				pcap_source_close(&ps); pcap_source_open(&ps, rx->cfg.source);
				have_counter = 0; wall_first_ns = 0; continue;
			}
		}
		if (n <= 0)
			continue;
		if (!reac_frame_is_reac(frame, (size_t)n))
			continue;

		/* optional: pace pcap replay by capture timestamps so the rate loop
		 * sees realistic cadence offline */
		if (!live && rx->cfg.pcap_realtime && pcap_ts) {
			if (!wall_first_ns) { wall_first_ns = mono_ns(); pcap_first_ts = pcap_ts; }
			uint64_t target = wall_first_ns + (pcap_ts - pcap_first_ts) * 1000ull;
			struct timespec ts = { target / 1000000000ull, target % 1000000000ull };
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
		}

		uint16_t counter = reac_frame_counter(frame);
		if (have_counter) {
			uint16_t gap = reac_counter_gap(last_counter, counter);
			if (gap)
				atomic_fetch_add_explicit(&rx->counter_gaps, gap, memory_order_relaxed);
		}
		last_counter = counter;
		have_counter = 1;

		update_ppm(rx, counter, mono_ns());
		feed_frame(rx, mode, frame, (size_t)n);
	}

	if (live)
		reac_capture_close(&cap);
	else
		pcap_source_close(&ps);
	return NULL;
}

int reac_rx_open(struct reac_rx *rx, const struct reac_rx_cfg *cfg, struct reac_ring *ring)
{
	memset(rx, 0, sizeof *rx);
	rx->cfg = *cfg;
	rx->ring = ring;

	if (cfg->forced_rate) {
		rx->sample_rate = cfg->forced_rate;
	} else if (cfg->kind == REAC_RX_LIVE) {
		struct reac_capture cap = { .fd = -1 };
		if (reac_capture_open(&cap, cfg->source) != 0)
			return -1;
		int r = reac_detect_rate_fd(cap.fd, 500);
		reac_capture_close(&cap);
		rx->sample_rate = r > 0 ? r : 48000; /* default when no traffic yet */
	} else {
		/* offline: peek a few frames to snap the rate from inter-arrival cadence
		 * would need timestamps; for pcap we accept forced_rate or default 48k. */
		rx->sample_rate = 48000;
	}

	/* ~250 ms of ring at the recovered rate, power-of-two rounded inside init */
	uint32_t depth = (uint32_t)(rx->sample_rate / 4);
	if (reac_ring_init(ring, REAC_MAX_CHANNELS, depth) != 0)
		return -1;
	return 0;
}

int reac_rx_start(struct reac_rx *rx)
{
	/* The feeder thread owns the wire source (opened in rx_loop). reac_rx_open
	 * already detected the rate + sized the ring; here we just flip running and
	 * spawn. This is the PRODUCER thread — plain SCHED_OTHER. Only the audio
	 * graph (and, in Phase 2, the TX slot pacer) run SCHED_FIFO. */
	atomic_store_explicit(&rx->running, 1, memory_order_release);
	return pthread_create(&rx->thread, NULL, rx_loop, rx);
}

void reac_rx_stop(struct reac_rx *rx)
{
	atomic_store_explicit(&rx->running, 0, memory_order_release);
	pthread_join(rx->thread, NULL);
}

void reac_rx_close(struct reac_rx *rx)
{
	(void)rx; /* ring is freed by the owner */
}
