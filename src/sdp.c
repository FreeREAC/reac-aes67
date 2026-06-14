// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "sdp.h"
#include <stdio.h>

/* AES67 / RFC 4566 SDP for an L24 multicast stream. With ts_refclk set it also
 * emits the RFC 7273 clock-source signalling (a=ts-refclk / a=mediaclk) that
 * Dante and strict AES67 receivers expect; without it, the minimal AES67 SDP
 * the PipeWire path uses. ptime defaults to 1 ms (the AES67/Dante mandatory). */
long sdp_build(char *out, size_t cap, const struct sdp_params *p)
{
	int ptime = p->ptime_ms > 0 ? p->ptime_ms : 1;
	size_t off = 0;
	int n;

	n = snprintf(out + off, cap - off,
		"v=0\r\n"
		"o=- 1 1 IN IP4 %s\r\n"
		"s=%s\r\n"
		"c=IN IP4 %s/%d\r\n"
		"t=0 0\r\n"
		"m=audio %d RTP/AVP %d\r\n"
		"a=rtpmap:%d L24/%d/%d\r\n"
		"a=ptime:%d\r\n",
		p->origin_ip, p->session_name,
		p->mcast_ip, p->ttl,
		p->mcast_port, p->payload_type,
		p->payload_type, p->rate, p->channels, ptime);
	if (n < 0 || (size_t)n >= cap - off)
		return -1;
	off += (size_t)n;

	/* RFC 7273 clock signalling (Dante / strict AES67). gmid:domain is carried
	 * verbatim in ts_refclk, e.g. "00-11-22-FF-FE-33-44-55:0". */
	if (p->ts_refclk) {
		n = snprintf(out + off, cap - off,
			"a=ts-refclk:ptp=IEEE1588-2008:%s\r\n"
			"a=mediaclk:direct=%d\r\n",
			p->ts_refclk, p->mediaclk_direct);
		if (n < 0 || (size_t)n >= cap - off)
			return -1;
		off += (size_t)n;
	}

	n = snprintf(out + off, cap - off, "a=recvonly\r\n");
	if (n < 0 || (size_t)n >= cap - off)
		return -1;
	off += (size_t)n;

	return (long)off;
}
