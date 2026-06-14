// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// Portions derived from obs-h8819-source, Copyright (C) 2022 Norihiro Kamae <norihiro@nagater.net> (GPL-3.0-or-later).
// REAC wire framing per github.com/per-gron/reacdriver (GPL-3.0).

#include "reac_decode.h"

const struct reac_mode REAC_MODE_48K = { 48000, 40, 12 };
/* 96k model SETTLED (2026-06-06) — candidate (a) double-pps {96000,40,12}.
 * On-rig the 96k stream measured ~8000 pps carrying the same 1492 B frame
 * (1440 B audio = 40 ch x 12 samples x 3 B), matching reacdriver REACConstants.h
 * (SAMPLES_PER_PACKET=12, PACKETS_PER_SECOND=8000, MAX_CHANNEL_COUNT=40). The
 * channel-halving model (b) {96000,20,24} would have been ~4000 pps / 20 ch;
 * both the measured pps and the byte layout refuted it. Frame is rate-invariant;
 * rate = pps x 12. */
const struct reac_mode REAC_MODE_96K = { 96000, 40, 12 };

struct reac_frame reac_frame_inspect(const uint8_t *raw, size_t len,
                                     const struct reac_mode *mode)
{
	(void)mode;
	struct reac_frame f = { 0, 0 };
	if (len != (size_t)REAC_FRAME_BYTES)
		return f;
	/* ethertype at bytes 12-13 (after dst6+src6) */
	if (raw[12] != 0x88 || raw[13] != 0x19)
		return f;
	/* end marker */
	if (raw[len - 2] != REAC_END_MARKER_0 || raw[len - 1] != REAC_END_MARKER_1)
		return f;
	f.counter = (uint16_t)(raw[14] | (raw[15] << 8)); /* l2_counter, LE */
	f.valid = 1;
	return f;
}

/* De-interleave the 1440 B audio region into planar 24-bit LE PCM.
 *
 * REAC packs audio sample-major and little-endian: for each of the 12 time-
 * samples all n_channels appear in order, each a 3-byte LE 24-bit value, so
 * channel ch / time-sample s starts at (s*n_channels + ch)*RESOLUTION and the
 * de-interleave is a straight copy.
 *
 * NOTE: this is DELIBERATELY not obs-h8819-source's convert_to_pcm24lep, which
 * braids each even/odd channel pair across its 6-byte group (even = bytes
 * 3,0,1; odd = 4,5,2). That braid is faithful to the device obs-h8819 targets,
 * but it scrambles the M-5000's payload into noise — verified on-rig 2026-06-06
 * by decoding a live M-5000 stream both ways (plain LE: coherent, coherence
 * 0.999; obs-h8819 braid: noise). REAC's wire endianness is common across
 * devices, but the in-payload channel-pair byte layout is NOT identical across
 * Roland generations; this decoder targets the M-5000's plain sequential layout.
 */
int reac_decode(const uint8_t *raw, size_t len, const struct reac_mode *mode,
                uint8_t *out)
{
	struct reac_frame f = reac_frame_inspect(raw, len, mode);
	if (!f.valid)
		return -1;

	const uint8_t *audio = raw + REAC_L2_HEADER_LEN;
	const int nch = mode->n_channels;
	const int ns = mode->samples_per_pkt;

	uint8_t *dptr = out;
	for (int ch = 0; ch < nch; ch++) {
		for (int s = 0; s < ns; s++) {
			const uint8_t *sptr = audio + (size_t)(s * nch + ch) * REAC_RESOLUTION;
			*dptr++ = sptr[0];
			*dptr++ = sptr[1];
			*dptr++ = sptr[2];
		}
	}
	return ns;
}
