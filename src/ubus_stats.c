// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ubus binding. Compiled with libubus on-device (-DHAVE_UBUS); elsewhere the
 * stubs below let the daemon + tests build without libubus headers. */
#define _GNU_SOURCE      /* libubox/uloop.h uses struct sigaction (POSIX) */
#define _DEFAULT_SOURCE
#include "ubus_stats.h"

static const char *tier_name(int t)
{
	switch (t) {
	case PLC_TIER_REPEAT_XFADE: return "crossfade";
	case PLC_TIER_BURST_FADE:   return "burst_fade";
	case PLC_TIER_HOLD_LAST:    return "hold_last";
	case PLC_TIER_SILENCE:      return "silence";
	default:                    return "clean";
	}
}

#ifdef HAVE_UBUS

#include <string.h>
#include <stdlib.h>
#include <libubus.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>

struct ubus_stats {
	struct ubus_context *ctx;
	struct ubus_object obj;
	struct ubus_object_type obj_type;
	struct ubus_method methods[1];
	char objname[64];
	/* static meta */
	char name[32], iface[16], mcast[32], ssrc[16];
	int rate, channels;
	/* latest snapshot */
	struct pipeline_stats stats;
	int pps;
	unsigned uptime;
	/* event-loop wiring (ubus_stats_run) */
	struct uloop_fd cap_ufd;
	struct uloop_timeout pub_timer;
	const struct ubus_stats_loop_cb *cb;
	unsigned pub_interval_s;
};

static struct blob_buf b;

static int status_cb(struct ubus_context *ctx, struct ubus_object *obj,
                     struct ubus_request_data *req, const char *method,
                     struct blob_attr *msg)
{
	struct ubus_stats *h = container_of(obj, struct ubus_stats, obj);
	(void)method; (void)msg;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "name", h->name);
	blobmsg_add_string(&b, "iface", h->iface);
	blobmsg_add_u32(&b, "rate", h->rate);
	blobmsg_add_u32(&b, "channels", h->channels);
	blobmsg_add_string(&b, "mcast", h->mcast);
	blobmsg_add_string(&b, "ssrc", h->ssrc);
	blobmsg_add_u8(&b, "running", 1);
	blobmsg_add_u32(&b, "uptime_s", h->uptime);
	blobmsg_add_u32(&b, "packets_per_sec", h->pps);
	blobmsg_add_u32(&b, "rtp_seq", h->stats.last_seq);
	blobmsg_add_u64(&b, "loss_total", h->stats.loss_total);
	blobmsg_add_u64(&b, "packets", h->stats.packets);
	blobmsg_add_u64(&b, "concealed", h->stats.concealed);
	blobmsg_add_string(&b, "plc_tier", tier_name(h->stats.last_tier));
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

struct ubus_stats *ubus_stats_start(const char *name, const char *iface,
                                    int rate, int channels, const char *mcast,
                                    const char *ssrc)
{
	struct ubus_stats *h = calloc(1, sizeof *h);
	if (!h) return NULL;
	h->ctx = ubus_connect(NULL);
	if (!h->ctx) { free(h); return NULL; }

	strncpy(h->name, name ? name : "reac", sizeof h->name - 1);
	strncpy(h->iface, iface ? iface : "", sizeof h->iface - 1);
	strncpy(h->mcast, mcast ? mcast : "", sizeof h->mcast - 1);
	strncpy(h->ssrc, ssrc ? ssrc : "", sizeof h->ssrc - 1);
	h->rate = rate; h->channels = channels;

	h->methods[0] = (struct ubus_method)UBUS_METHOD_NOARG("status", status_cb);
	h->obj_type.name = "reac-aes67";
	h->obj_type.methods = h->methods;
	h->obj_type.n_methods = 1;
	snprintf(h->objname, sizeof h->objname, "reac-aes67.%s", h->name);
	h->obj.name = h->objname;
	h->obj.type = &h->obj_type;
	h->obj.methods = h->methods;
	h->obj.n_methods = 1;

	if (ubus_add_object(h->ctx, &h->obj) != 0) {
		ubus_free(h->ctx);
		free(h);
		return NULL;
	}
	return h;
}

void ubus_stats_update(struct ubus_stats *h, const struct pipeline_stats *s,
                       int packets_per_sec, unsigned uptime_s)
{
	if (!h) return;
	h->stats = *s;
	h->pps = packets_per_sec;
	h->uptime = uptime_s;
}

void ubus_stats_poll(struct ubus_stats *h)
{
	if (!h) return;
	ubus_handle_event(h->ctx); /* drain pending (non-uloop callers) */
}

/* uloop: capture fd readable -> let the caller drain frames into the pipeline */
static void on_cap_uloop(struct uloop_fd *u, unsigned int events)
{
	(void)events;
	struct ubus_stats *h = container_of(u, struct ubus_stats, cap_ufd);
	if (h->cb && h->cb->on_capture_readable)
		h->cb->on_capture_readable(h->cb->ctx);
}

/* uloop: periodic publish tick -> caller computes stats + calls ubus_stats_update */
static void on_pub_uloop(struct uloop_timeout *t)
{
	struct ubus_stats *h = container_of(t, struct ubus_stats, pub_timer);
	if (h->cb && h->cb->on_publish)
		h->cb->on_publish(h->cb->ctx);
	uloop_timeout_set(&h->pub_timer, (int)h->pub_interval_s * 1000);
}

int ubus_stats_run(struct ubus_stats *h, int capture_fd,
                   const struct ubus_stats_loop_cb *cb, unsigned publish_interval_s)
{
	if (!h) return -1;
	h->cb = cb;
	h->pub_interval_s = publish_interval_s ? publish_interval_s : 1;

	uloop_init();
	ubus_add_uloop(h->ctx);                 /* service the ubus socket in uloop */

	h->cap_ufd.fd = capture_fd;
	h->cap_ufd.cb = on_cap_uloop;
	uloop_fd_add(&h->cap_ufd, ULOOP_READ);  /* capture fd readable wakes the loop */

	h->pub_timer.cb = on_pub_uloop;
	uloop_timeout_set(&h->pub_timer, (int)h->pub_interval_s * 1000);

	uloop_run();                            /* runs until uloop_end() (signal) */

	uloop_fd_delete(&h->cap_ufd);
	uloop_timeout_cancel(&h->pub_timer);
	uloop_done();
	return 0;
}

void ubus_stats_stop(struct ubus_stats *h)
{
	if (!h) return;
	ubus_free(h->ctx);
	free(h);
}

#else /* no libubus: stubs keep the daemon + tests building */

struct ubus_stats *ubus_stats_start(const char *name, const char *iface,
                                    int rate, int channels, const char *mcast,
                                    const char *ssrc)
{
	(void)name; (void)iface; (void)rate; (void)channels; (void)mcast; (void)ssrc;
	(void)tier_name; /* silence unused-static when stubbed */
	return 0;
}
void ubus_stats_update(struct ubus_stats *h, const struct pipeline_stats *s,
                       int packets_per_sec, unsigned uptime_s)
{ (void)h; (void)s; (void)packets_per_sec; (void)uptime_s; }
void ubus_stats_poll(struct ubus_stats *h) { (void)h; }
int ubus_stats_run(struct ubus_stats *h, int capture_fd,
                   const struct ubus_stats_loop_cb *cb, unsigned publish_interval_s)
{ (void)h; (void)capture_fd; (void)cb; (void)publish_interval_s; return -1; }
void ubus_stats_stop(struct ubus_stats *h) { (void)h; }

#endif
