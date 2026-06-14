// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#define _DEFAULT_SOURCE
#include "aes67_send.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>

/* Resolve an interface name to its primary IPv4 address (for IP_MULTICAST_IF).
 * Returns 0 + fills addr on success, -1 otherwise. */
static int ifname_to_addr(int fd, const char *ifname, struct in_addr *addr)
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFADDR, &ifr) < 0)
		return -1;
	*addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
	return 0;
}

int aes67_send_open(struct aes67_sender *s, const char *ip, uint16_t port,
                    int ttl, const char *ifname)
{
	s->fd = -1;
	s->dest = NULL;

	struct sockaddr_in *dst = calloc(1, sizeof *dst);
	if (!dst)
		return -1;
	dst->sin_family = AF_INET;
	dst->sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &dst->sin_addr) != 1) {
		free(dst);
		return -1;
	}

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		free(dst);
		return -1;
	}

	/* multicast TTL (harmless for unicast). */
	unsigned char t = (unsigned char)(ttl < 1 ? 1 : ttl);
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &t, sizeof t);
	/* enable multicast loopback so a receiver on the SAME host sees the stream
	 * (needed for the local demo; harmless otherwise). */
	unsigned char loop = 1;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);

	/* Pin multicast egress to a chosen interface, so packets leave the intended
	 * segment (e.g. the LAN/aes67 bridge) rather than following the default
	 * route (which on a router goes out WAN). */
	if (ifname && *ifname) {
		struct in_addr ia;
		if (ifname_to_addr(fd, ifname, &ia) == 0)
			setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof ia);
		/* SO_BINDTODEVICE as a belt-and-suspenders (needs CAP_NET_RAW; ignore
		 * failure — IP_MULTICAST_IF above is the primary mechanism). */
		setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, (socklen_t)strlen(ifname));
	}

	s->fd = fd;
	s->dest = dst;
	return 0;
}

int aes67_send_set_dscp(struct aes67_sender *s, int dscp)
{
	if (s->fd < 0)
		return -1;
	/* DSCP occupies the high 6 bits of the 8-bit IPv4 TOS/DS field. */
	int tos = (dscp & 0x3f) << 2;
	return setsockopt(s->fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
}

long aes67_send_packet(struct aes67_sender *s, const uint8_t *buf, size_t len)
{
	if (s->fd < 0 || !s->dest)
		return -1;
	const struct sockaddr_in *dst = s->dest;
	return sendto(s->fd, buf, len, 0, (const struct sockaddr *)dst, sizeof *dst);
}

void aes67_send_close(struct aes67_sender *s)
{
	if (s->fd >= 0) {
		close(s->fd);
		s->fd = -1;
	}
	free(s->dest);
	s->dest = NULL;
}
