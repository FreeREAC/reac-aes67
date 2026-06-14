// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "sap.h"
#include <string.h>
#include <arpa/inet.h>

#define SAP_MIME "application/sdp"

long sap_build(uint8_t *out, size_t cap, const char *origin_ip,
               uint16_t msg_id_hash, const char *sdp)
{
	struct in_addr a;
	if (inet_pton(AF_INET, origin_ip, &a) != 1)
		return -1;

	size_t mime_len = sizeof(SAP_MIME);      /* includes the trailing NUL */
	size_t sdp_len = strlen(sdp);
	size_t total = 8 + mime_len + sdp_len;
	if (cap < total)
		return -1;

	out[0] = 0x20;                            /* V=1 (001<<5), announce, IPv4 */
	out[1] = 0x00;                            /* auth length = 0 */
	out[2] = (uint8_t)(msg_id_hash >> 8);
	out[3] = (uint8_t)(msg_id_hash & 0xff);
	memcpy(out + 4, &a.s_addr, 4);            /* origin IPv4, network order */

	memcpy(out + 8, SAP_MIME, mime_len);      /* "application/sdp\0" */
	memcpy(out + 8 + mime_len, sdp, sdp_len); /* SDP body, no trailing NUL */
	return (long)total;
}
