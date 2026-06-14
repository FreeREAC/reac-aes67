// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac:playback — Audio/Sink that encodes the graph's PCM into REAC downstream
 * frames and emits them on a raw 0x8819 socket (reac_tx). First functional cut:
 *
 *   - registers an Audio/Sink with `channels` mono DSP input ports, so apps
 *     (Rhythmbox, pw-play, ...) and the graph can play INTO it;
 *   - process() de-stages the quantum into 12-sample REAC frames and emits each
 *     via reac_tx_emit; the wire cadence is therefore tied to the graph clock
 *     (every 12 input samples -> one frame -> rate = graph_rate, e.g. 4000 fps
 *     at 48 k). PipeWire resamples whatever app rate into this node's rate.
 *
 * Scope of this cut (loopback demo): it emits a DOWNSTREAM MASTER broadcast that
 * our own reac:capture (or a pcap) decodes. It does NOT drive the JOIN/HOLD
 * connection FSM, so a real Roland desk will not link to it — that, plus the
 * SCHED_FIFO slot pacer (to take the sendto() syscall off the RT graph thread),
 * is the remaining work. See NATIVE-REAC-DESIGN.md S2/S6. The sendto() in
 * process() is a deliberate first-cut wart, fine over a local veth/lo loopback. */

#include "reac_sink_node.h"
#include "reac_tx.h"

#include <reac/reac.h>
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <stdlib.h>
#include <string.h>

struct port_in { /* per-port user data PipeWire hands back */
	int channel;
};

struct reac_sink_node {
	struct pw_filter *filter;
	struct reac_tx tx;
	int tx_open;
	int channels;
	int sample_rate;
	struct port_in *ports[REAC_MAX_CHANNELS];

	/* 12-sample-per-channel staging accumulator: a PipeWire quantum is not a
	 * multiple of REAC_SAMPLES_PER_PKT, so we carry the remainder across cycles
	 * and emit a frame each time the stage fills. */
	float stage[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	int staged; /* 0..REAC_SAMPLES_PER_PKT-1 */
};

/* REALTIME. Pull this quantum's PCM from the input ports, accumulate into the
 * 12-sample stage, and emit a REAC frame for every full group. */
static void on_process(void *data, struct spa_io_position *position)
{
	struct reac_sink_node *n = data;
	if (!n->tx_open)
		return;
	uint32_t nframes = position->clock.duration;

	const float *in[REAC_MAX_CHANNELS];
	int have = 0;
	for (int c = 0; c < n->channels; c++) {
		float *b = pw_filter_get_dsp_buffer(n->ports[c], nframes);
		in[c] = b;            /* NULL if this port is unlinked this cycle */
		if (b)
			have++;
	}
	if (have == 0)
		return;               /* nothing feeding us — emit nothing (silence) */

	float *planar[REAC_MAX_CHANNELS];
	for (int c = 0; c < n->channels; c++)
		planar[c] = n->stage[c];

	for (uint32_t s = 0; s < nframes; s++) {
		for (int c = 0; c < n->channels; c++)
			n->stage[c][n->staged] = in[c] ? in[c][s] : 0.0f;
		if (++n->staged == REAC_SAMPLES_PER_PKT) {
			reac_tx_emit(&n->tx, planar, n->channels, REAC_SAMPLES_PER_PKT);
			n->staged = 0;
		}
	}
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
};

struct reac_sink_node *reac_sink_node_new(struct pw_loop *loop,
                                          struct reac_ring *tx_ring,
                                          const struct reac_sink_cfg *cfg)
{
	(void)tx_ring; /* direct-emit cut: no ring yet (a pacer thread would use it) */

	struct reac_sink_node *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	n->channels = cfg->channels > REAC_MAX_CHANNELS ? REAC_MAX_CHANNELS : cfg->channels;
	n->sample_rate = cfg->sample_rate;

	if (reac_tx_open(&n->tx, cfg->ifname) != 0) {
		pw_log_warn("reac:playback — cannot open AF_PACKET TX on '%s' "
		            "(needs CAP_NET_RAW + a valid interface); sink not created",
		            cfg->ifname);
		free(n);
		return NULL;
	}
	n->tx_open = 1;
	if (cfg->src_mac)
		memcpy(n->tx.src, cfg->src_mac, 6);

	char rate_str[16];
	snprintf(rate_str, sizeof rate_str, "1/%d", n->sample_rate);

	n->filter = pw_filter_new_simple(
		loop,
		"reac:playback",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback", /* a sink consumes audio */
			PW_KEY_MEDIA_CLASS, "Audio/Sink",  /* shows up as an output device */
			PW_KEY_NODE_NAME, "reac-playback",
			PW_KEY_NODE_DESCRIPTION, "REAC 40ch playback (downstream master TX)",
			/* The wire is the rate authority; advertise the REAC rate so PipeWire
			 * resamples whatever the app plays into our pps. */
			PW_KEY_NODE_RATE, rate_str,
			NULL),
		&filter_events, n);
	if (!n->filter) {
		reac_tx_close(&n->tx);
		free(n);
		return NULL;
	}

	for (int c = 0; c < n->channels; c++) {
		char pname[24];
		snprintf(pname, sizeof pname, "playback_%02d", c + 1);
		n->ports[c] = pw_filter_add_port(
			n->filter,
			PW_DIRECTION_INPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS,
			sizeof(struct port_in),
			pw_properties_new(
				PW_KEY_FORMAT_DSP, "32 bit float mono audio",
				PW_KEY_PORT_NAME, pname,
				NULL),
			NULL, 0);
		if (n->ports[c])
			n->ports[c]->channel = c;
	}

	if (pw_filter_connect(n->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
		pw_filter_destroy(n->filter);
		reac_tx_close(&n->tx);
		free(n);
		return NULL;
	}

	pw_log_info("reac:playback emitting REAC 0x8819 on '%s' (%d ch, %d Hz)",
	            cfg->ifname, n->channels, n->sample_rate);
	return n;
}

void reac_sink_node_destroy(struct reac_sink_node *n)
{
	if (!n)
		return;
	if (n->filter)
		pw_filter_destroy(n->filter);
	if (n->tx_open)
		reac_tx_close(&n->tx);
	free(n);
}
