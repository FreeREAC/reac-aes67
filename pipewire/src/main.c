// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac-pw — PipeWire-native REAC endpoint (RX SOURCE node; TX SINK scaffolded).
 *
 * A single libpipewire client that registers the 40-channel REAC source node
 * fed by a pcap replay (offline) or a live AF_PACKET 0x8819 socket, decoding
 * with the reac-aes67 plain-LE core and pushing samples through a lock-free
 * ring into the realtime process() callback. The node is a follower; PipeWire's
 * adapter resamples REAC -> graph (Tier-A clock bridge). The TX sink node is
 * registered as an interface-only skeleton (libreac TX layer unbuilt).
 *
 *   reac-pw --pcap  capture.pcap [--rate 48000]
 *   reac-pw --live  reac0        [--rate 96000]
 */

#include "reac_ring.h"
#include "reac_rx.h"
#include "reac_source_node.h"
#include "reac_sink_node.h"

#include <pipewire/pipewire.h>
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

static struct pw_main_loop *g_loop;

static void on_signal(void *data, int sig)
{
	(void)data; (void)sig;
	pw_main_loop_quit(g_loop);
}

static void usage(const char *p)
{
	fprintf(stderr,
	  "usage: %s (--pcap FILE | --live IFNAME) [--rate 44100|48000|96000] [--tx IFNAME]\n"
	  "  --pcap FILE   replay a REAC capture (offline test, reuses pcap_source)\n"
	  "  --live IFNAME live AF_PACKET 0x8819 capture (reuses reac_capture; needs CAP_NET_RAW)\n"
	  "  --rate R      force the REAC sample rate (default: auto-detect on --live, 48000 on --pcap)\n"
	  "  --tx IFNAME   register the reac:playback sink (encodes + emits REAC 0x8819) on this NIC\n", p);
}

int main(int argc, char **argv)
{
	struct reac_rx_cfg rxcfg = { .kind = REAC_RX_PCAP, .source = NULL, .forced_rate = 0,
	                             .pcap_realtime = 1 };
	const char *tx_if = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pcap") && i + 1 < argc) {
			rxcfg.kind = REAC_RX_PCAP; rxcfg.source = argv[++i];
		} else if (!strcmp(argv[i], "--live") && i + 1 < argc) {
			rxcfg.kind = REAC_RX_LIVE; rxcfg.source = argv[++i];
		} else if (!strcmp(argv[i], "--rate") && i + 1 < argc) {
			rxcfg.forced_rate = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--tx") && i + 1 < argc) {
			tx_if = argv[++i];
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	if (!rxcfg.source) {
		usage(argv[0]);
		return 2;
	}

	pw_init(&argc, &argv);

	struct reac_ring ring;
	struct reac_rx rx;
	if (reac_rx_open(&rx, &rxcfg, &ring) != 0) {
		fprintf(stderr, "reac-pw: cannot open source '%s'\n", rxcfg.source);
		return 1;
	}
	fprintf(stderr, "reac-pw: recovered REAC rate = %d Hz (%d pps), 40 ch\n",
	        rx.sample_rate, rx.sample_rate / REAC_SAMPLES_PER_PKT);

	g_loop = pw_main_loop_new(NULL);
	struct pw_loop *loop = pw_main_loop_get_loop(g_loop);
	pw_loop_add_signal(loop, SIGINT, on_signal, NULL);
	pw_loop_add_signal(loop, SIGTERM, on_signal, NULL);

	struct reac_source_node *src = reac_source_node_new(loop, &ring, &rx, rx.sample_rate);
	if (!src) {
		fprintf(stderr, "reac-pw: failed to create reac:capture node\n");
		return 1;
	}

	struct reac_sink_node *sink = NULL;
	struct reac_ring tx_ring;
	if (tx_if) {
		static const uint8_t roland_oui_mac[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
		reac_ring_init(&tx_ring, REAC_MAX_CHANNELS, (uint32_t)(rx.sample_rate / 4));
		struct reac_sink_cfg scfg = { .ifname = tx_if, .channels = REAC_MAX_CHANNELS,
		                              .sample_rate = rx.sample_rate,
		                              .src_mac = roland_oui_mac, .master_mac = NULL };
		sink = reac_sink_node_new(loop, &tx_ring, &scfg); /* encodes + emits REAC */
		if (!sink)
			fprintf(stderr, "reac-pw: reac:playback sink not created "
			        "(TX socket on '%s' failed — need CAP_NET_RAW?)\n", tx_if);
	}

	if (reac_rx_start(&rx) != 0) {
		fprintf(stderr, "reac-pw: failed to start RX feeder\n");
		return 1;
	}

	pw_main_loop_run(g_loop);

	reac_rx_stop(&rx);
	reac_source_node_destroy(src);
	reac_sink_node_destroy(sink);
	if (tx_if)
		reac_ring_free(&tx_ring);
	reac_rx_close(&rx);
	reac_ring_free(&ring);
	pw_main_loop_destroy(g_loop);
	pw_deinit();
	return 0;
}
