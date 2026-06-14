// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "rtp_l24.h"

/* RFC 3550 fixed header, big-endian. V=2, P=0, X=0, CC=0, M=0. */
static void write_rtp_header(uint8_t *out, const struct rtp_params *p)
{
	out[0] = (uint8_t)(RTP_VERSION << 6);
	out[1] = (uint8_t)(p->payload_type & 0x7f);
	out[2] = (uint8_t)(p->seq >> 8);
	out[3] = (uint8_t)(p->seq & 0xff);
	out[4] = (uint8_t)(p->timestamp >> 24);
	out[5] = (uint8_t)(p->timestamp >> 16);
	out[6] = (uint8_t)(p->timestamp >> 8);
	out[7] = (uint8_t)(p->timestamp & 0xff);
	out[8] = (uint8_t)(p->ssrc >> 24);
	out[9] = (uint8_t)(p->ssrc >> 16);
	out[10] = (uint8_t)(p->ssrc >> 8);
	out[11] = (uint8_t)(p->ssrc & 0xff);
}

long rtp_l24_build_flow(uint8_t *out, size_t cap,
                        const struct rtp_params *p,
                        const uint8_t *planar_pcm,
                        int total_nch, int ch_start, int flow_nch,
                        int n_samples)
{
	if (ch_start < 0 || flow_nch <= 0 || n_samples <= 0 ||
	    ch_start + flow_nch > total_nch)
		return -1;
	const size_t payload = (size_t)flow_nch * (size_t)n_samples * 3;
	const size_t total = RTP_HEADER_LEN + payload;
	if (cap < total)
		return -1;

	write_rtp_header(out, p);

	/* planar LE (channel-major, total_nch x n_samples) -> interleaved BE
	 * (frame-major, flow_nch channels). src sample (ch, s) is at
	 * planar_pcm[(ch*n_samples + s)*3] LE; dst sample (s, fc) is at
	 * payload + (s*flow_nch + fc)*3 BE, where fc = ch - ch_start. */
	uint8_t *a = out + RTP_HEADER_LEN;
	for (int s = 0; s < n_samples; s++) {
		for (int fc = 0; fc < flow_nch; fc++) {
			int ch = ch_start + fc;
			const uint8_t *src = planar_pcm + (size_t)(ch * n_samples + s) * 3;
			uint8_t *dst = a + (size_t)(s * flow_nch + fc) * 3;
			dst[0] = src[2]; /* BE high  <- LE high */
			dst[1] = src[1];
			dst[2] = src[0]; /* BE low   <- LE low  */
		}
	}
	return (long)total;
}

long rtp_l24_build(uint8_t *out, size_t cap,
                   const struct rtp_params *p,
                   const uint8_t *planar_pcm,
                   const struct reac_mode *mode)
{
	return rtp_l24_build_flow(out, cap, p, planar_pcm,
	                          mode->n_channels, 0, mode->n_channels,
	                          mode->samples_per_pkt);
}
