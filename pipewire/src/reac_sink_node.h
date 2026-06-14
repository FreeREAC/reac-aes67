// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_sink_node — the virtual-stagebox TX sink node.
 *
 * FUNCTIONAL first cut (the reac_tx encoder it needs now exists, src/reac_tx.c):
 *   - pw_filter registered Audio/Sink with N INPUT ports (the box's input count)
 *   - realtime process() de-stages each quantum into 12-sample REAC frames and
 *     emits them via reac_tx_emit on a raw 0x8819 socket. The wire cadence is
 *     tied to the graph clock (12 input samples -> one frame); PipeWire
 *     resamples the app rate INTO our advertised REAC rate.
 *
 * NOT yet done (loopback demo only — our TX is decoded by our own reac:capture,
 * not a real desk):
 *   - the SCHED_FIFO slot pacer (the reac_repacer.c pattern: prio ~79, mlockall,
 *     affinity, clock_nanosleep TIMER_ABSTIME) that would take the sendto()
 *     syscall off the RT graph thread and clock frames at a fixed pps;
 *   - the JOIN/HOLD connection FSM (flood FILLER on PHY-up -> cold-connect
 *     cdea 04 03 -> config-announce -> heartbeat cdea 01 03 0001 81 at ~1/s ->
 *     walk the 40-ch map, never deduped -> stable src-MAC). Without it a real
 *     Roland desk will not link to us.
 * See NATIVE-REAC-DESIGN.md Section 2 + Section 6.
 */
#ifndef REAC_SINK_NODE_H
#define REAC_SINK_NODE_H

#include "reac_ring.h"

struct pw_loop;
struct reac_sink_node;

struct reac_sink_cfg {
	const char *ifname;   /* TX NIC (raw AF_PACKET 0x8819) */
	int channels;         /* the box's input count (<= 40) */
	int sample_rate;      /* fixed pps authority: 44100/48000/96000 */
	const uint8_t *src_mac;   /* stable Roland-OUI src MAC for the virtual box */
	const uint8_t *master_mac;/* unicast destination once linked */
};

/* Create the sink node. Opens an AF_PACKET 0x8819 TX socket on cfg->ifname
 * (needs CAP_NET_RAW) and registers cfg->channels INPUT ports; process() encodes
 * + emits. Returns NULL (and logs) if the TX socket can't open. `tx_ring` is
 * reserved for the future slot-pacer cut; the direct-emit path ignores it. */
struct reac_sink_node *reac_sink_node_new(struct pw_loop *loop,
                                          struct reac_ring *tx_ring,
                                          const struct reac_sink_cfg *cfg);

void reac_sink_node_destroy(struct reac_sink_node *n);

#endif /* REAC_SINK_NODE_H */
