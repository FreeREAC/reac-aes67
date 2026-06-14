// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* sdp — render an AES67 SDP session description for a REAC stream.
 *
 * Pure string builder (no I/O), so it is unit-testable and shared by the daemon
 * (ubus/status) and any tooling. Produces a minimal RFC 4566 / AES67 SDP for an
 * L24 multicast stream.
 */
#ifndef SDP_H
#define SDP_H

#include <stddef.h>

struct sdp_params {
	const char *session_name; /* e.g. "REAC-A" */
	const char *origin_ip;    /* source/origin IPv4 (e.g. the router's addr) */
	const char *mcast_ip;     /* destination multicast group */
	int mcast_port;
	int payload_type;         /* dynamic PT */
	int rate;                 /* 48000 / 96000 */
	int channels;             /* 40 / 20 / 8 (per-flow for Dante) */
	int ttl;                  /* multicast TTL */
	/* --- optional; 0/NULL reproduce the minimal AES67 SDP --- */
	int ptime_ms;             /* packet time in ms; 0 -> 1 (the AES67/Dante default) */
	const char *ts_refclk;    /* RFC 7273: "<gmid>:<domain>" e.g.
	                           * "00-11-22-FF-FE-33-44-55:0". NULL -> omit the
	                           * a=ts-refclk/a=mediaclk lines (plain AES67). */
	int mediaclk_direct;      /* RFC 7273 a=mediaclk:direct=<n> (only if ts_refclk) */
};

/* Write the SDP text into out (capacity cap). Returns the length written
 * (excluding the NUL), or -1 if cap is too small. */
long sdp_build(char *out, size_t cap, const struct sdp_params *p);

#endif /* SDP_H */
