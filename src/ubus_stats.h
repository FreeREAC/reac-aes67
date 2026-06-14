// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ubus_stats — publish per-stream live stats over ubus for LuCI.
 *
 * Thin shell over the pure pipeline_stats core. Registers a ubus object
 * "reac-aes67.<name>" exposing a `status` method. Built only WITH libubus
 * (on OpenWrt); on hosts without libubus it compiles to
 * no-op stubs so the rest of the daemon and the test suite build unchanged.
 */
#ifndef UBUS_STATS_H
#define UBUS_STATS_H

#include "pipeline.h"

struct ubus_stats; /* opaque */

/* Connect to ubus and register object "reac-aes67.<name>". Returns a handle, or
 * NULL if ubus is unavailable / not compiled in. meta is static stream info
 * (iface/rate/channels/mcast) surfaced alongside the live counters. */
struct ubus_stats *ubus_stats_start(const char *name, const char *iface,
                                    int rate, int channels, const char *mcast,
                                    const char *ssrc);

/* Push the latest counters (call periodically or per N packets). No-op if the
 * handle is NULL. uptime_s and packets_per_sec are computed by the caller. */
void ubus_stats_update(struct ubus_stats *h, const struct pipeline_stats *s,
                       int packets_per_sec, unsigned uptime_s);

/* Service ubus I/O (call from the daemon loop, non-blocking). No-op if NULL. */
void ubus_stats_poll(struct ubus_stats *h);

/* Event-loop callbacks for ubus_stats_run (HAVE_UBUS builds). */
struct ubus_stats_loop_cb {
	void (*on_capture_readable)(void *ctx); /* capture fd readable: drain -> pipeline */
	void (*on_publish)(void *ctx);          /* timer tick: compute + ubus_stats_update */
	void *ctx;
};

/* Run a uloop servicing the ubus socket + the capture fd + a periodic publish
 * timer concurrently — the correct pattern so `ubus call` is answered even when
 * REAC traffic is idle. The caller's capture fd should be set non-blocking so
 * on_capture_readable can drain all ready frames and return.
 *
 * Returns 0 when the loop ends (signal), or -1 if not compiled with libubus/
 * uloop (the caller must then fall back to its own blocking loop). No-op
 * returning -1 if h is NULL. */
int ubus_stats_run(struct ubus_stats *h, int capture_fd,
                   const struct ubus_stats_loop_cb *cb, unsigned publish_interval_s);

void ubus_stats_stop(struct ubus_stats *h);

#endif /* UBUS_STATS_H */
