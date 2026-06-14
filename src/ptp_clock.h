// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* PTP/TAI media-clock helpers for AES67 senders that must lock to a shared
 * PTPv2 grandmaster (Dante interop, RFC 7273).
 *
 * The RTP timestamp of a PTP-locked AES67 stream is NOT free-running: with
 * a=mediaclk:direct=0 it is the media-clock sample count since the IEEE 1588
 * epoch (1970-01-01 00:00:00 TAI), so two senders on the same grandmaster are
 * sample-phase aligned. tai_to_rtp_ts() is the pure derivation; ptp_now_tai()
 * is the runtime read of the PTP-disciplined system clock (the integration
 * seam — ptp4l disciplines CLOCK_TAI on the router).
 */
#ifndef PTP_CLOCK_H
#define PTP_CLOCK_H

#include <stdint.h>

/* RFC 7273 mediaclk:direct=0 timestamp:
 *   RTP_ts = ( floor(tai_sec*rate) + round(tai_nsec*rate/1e9) ) mod 2^32.
 * Pure and deterministic — unit-tested with injected TAI values. */
uint32_t tai_to_rtp_ts(uint64_t tai_sec, uint32_t tai_nsec, int rate);

/* Read the PTP-disciplined TAI clock into the sec, nsec out-params (epoch 1970 TAI).
 * Returns 0 on success, -1 if CLOCK_TAI is unavailable. Runtime seam: real
 * PTP lock (ptp4l, slave-only) is validated on hardware. */
int ptp_now_tai(uint64_t *sec, uint32_t *nsec);

#endif /* PTP_CLOCK_H */
