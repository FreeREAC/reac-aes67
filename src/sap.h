// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* sap — SAP (RFC 2974) session announcement for AES67 auto-discovery.
 *
 * Builds a SAP announcement packet = 8-byte SAP header + the MIME type
 * "application/sdp\0" + the SDP body (from sdp_build). Receivers running a SAP
 * listener (e.g. PipeWire libpipewire-module-rtp-sap in discover mode) pick the
 * announcement up and auto-create the audio node — no per-receiver config.
 *
 * The packet builder is pure (no I/O) so the wire format is unit-testable; the
 * periodic multicast send to 224.0.0.56:9875 (the AES67/SAP group) is a thin
 * socket shell layered on aes67_send.
 */
#ifndef SAP_H
#define SAP_H

#include <stdint.h>
#include <stddef.h>

/* SAP/AES67 well-known announcement destination. 224.0.0.56 is PipeWire's
 * module-rtp-sap default (the existing AES67 receiver); Dante Controller listens
 * for AES67 SAP on 239.255.255.255. Both use port 9875. */
#define SAP_MCAST_ADDR "224.0.0.56"
#define SAP_MCAST_ADDR_DANTE "239.255.255.255"
#define SAP_PORT 9875

/* Build a SAPv1 IPv4 announce packet into out (capacity cap):
 *   byte0: 0x20  (V=1, A=0 IPv4, R=0, T=0 announce, E=0, C=0)
 *   byte1: 0x00  (auth length = 0)
 *   bytes2-3: 16-bit message id hash (big-endian)
 *   bytes4-7: originating source IPv4 (network order)
 *   then "application/sdp\0", then the SDP text (no trailing NUL).
 * origin_ip is dotted-quad; msg_id_hash should be stable per session (e.g.
 * derived from name+mcast) so receivers can dedupe/refresh.
 * Returns total packet length, or -1 if cap is too small / origin invalid. */
long sap_build(uint8_t *out, size_t cap, const char *origin_ip,
               uint16_t msg_id_hash, const char *sdp);

#endif /* SAP_H */
