// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#include "reac_capture.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>

#if defined(__linux__)
#include <linux/if_packet.h>
#include <linux/if_ether.h>

#ifndef ETH_P_REAC
#define ETH_P_REAC 0x8819
#endif

int reac_capture_open(struct reac_capture *c, const char *ifname)
{
	c->fd = -1;
	int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_REAC));
	if (fd < 0)
		return -1;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		close(fd);
		return -1;
	}

	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_REAC);
	sll.sll_ifindex = ifr.ifr_ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) {
		close(fd);
		return -1;
	}

	c->fd = fd;
	return 0;
}

int reac_capture_set_nonblock(struct reac_capture *c, int nonblock)
{
	if (c->fd < 0)
		return -1;
	int fl = fcntl(c->fd, F_GETFL, 0);
	if (fl < 0)
		return -1;
	fl = nonblock ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
	return fcntl(c->fd, F_SETFL, fl) < 0 ? -1 : 0;
}

long reac_capture_next(struct reac_capture *c, uint8_t *buf, size_t cap)
{
	if (c->fd < 0)
		return -1;
	for (;;) {
		ssize_t n = recv(c->fd, buf, cap, 0);
		if (n < 0) {
			/* EINTR: return 0 so the caller can re-check its stop flag and
			 * exit cleanly (a traffic-idle blocking --listen otherwise can't
			 * be signalled). EAGAIN/EWOULDBLOCK: non-blocking, nothing ready. */
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			return -1;
		}
		/* socket is bound to ETH_P_REAC, but double-check the ethertype. */
		if (n >= 14 && buf[12] == 0x88 && buf[13] == 0x19)
			return (long)n;
		/* non-REAC frame: in non-blocking mode yield (return 0) so we don't
		 * spin; in blocking mode keep reading for the next frame. */
		int fl = fcntl(c->fd, F_GETFL, 0);
		if (fl >= 0 && (fl & O_NONBLOCK))
			return 0;
	}
}

void reac_capture_close(struct reac_capture *c)
{
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
}

#else /* non-Linux: AF_PACKET unavailable */

int reac_capture_open(struct reac_capture *c, const char *ifname)
{
	(void)ifname;
	c->fd = -1;
	return -1;
}
int reac_capture_set_nonblock(struct reac_capture *c, int nonblock)
{
	(void)c; (void)nonblock;
	return -1;
}
long reac_capture_next(struct reac_capture *c, uint8_t *buf, size_t cap)
{
	(void)c; (void)buf; (void)cap;
	return -1;
}
void reac_capture_close(struct reac_capture *c) { (void)c; }

#endif
