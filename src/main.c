// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac-aes67 CLI / daemon.
 *
 * Modes:
 *   --pcap FILE   --dump-samples         decode + print samples (cross-verify)
 *   --pcap FILE   --count-rtp            run pipeline, count RTP packets
 *   --pcap FILE   --udp IP:PORT          replay -> pipeline -> AES67 (off-site test)
 *   --listen IF   --udp IP:PORT          LIVE: AF_PACKET capture -> pipeline -> AES67
 *
 * Common: --rate 48000|96000 (default 48000), --pt N (RTP payload type, 97),
 *         --ssrc HEX, --ttl N (multicast TTL, 1).
 *
 * The decode/clock/PLC/RTP pipeline is shared by all modes; only the frame
 * source (pcap vs AF_PACKET) and the sink (stdout vs UDP) differ.
 */
#define _POSIX_C_SOURCE 200809L  /* clock_gettime / clock_nanosleep */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "pcap_source.h"
#include "reac_capture.h"
#include "reac_decode.h"
#include "reac_mode.h"
#include "pipeline.h"
#include "aes67_send.h"
#include "ubus_stats.h"
#include "sdp.h"
#include "tone.h"
#include "rtp_l24.h"
#include "sap.h"
#include "packetizer.h"
#include "ptp_clock.h"
#include <time.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static int parse_ipport(const char *s, char *ip, size_t ipcap, uint16_t *port);

static const struct reac_mode *mode_for(int rate)
{
	return rate == 96000 ? &REAC_MODE_96K : &REAC_MODE_48K;
}

/* ---- emit callbacks ---- */
static void emit_udp(void *ctx, const uint8_t *buf, size_t len, int concealed)
{
	(void)concealed;
	aes67_send_packet((struct aes67_sender *)ctx, buf, len);
}

/* ---- SAP announcer (RFC 2974, AES67 auto-discovery) ----
 * Holds a sender to 224.0.0.56:9875 and a pre-built SAP packet (SAP header +
 * application/sdp + the stream's SDP). announce() re-sends it; receivers
 * running a SAP listener auto-create the audio node. Lives inside the daemon
 * (no separate process) and is driven by the existing periodic timer/loop. */
struct sap_announcer {
	struct aes67_sender snd;
	uint8_t pkt[1024];
	long len;
	int active;
};

static uint16_t sap_hash(const char *s)
{
	uint16_t h = 0x1234;
	for (; s && *s; s++) h = (uint16_t)(h * 31 + (unsigned char)*s);
	return h ? h : 1;
}

/* Build the SAP packet for this stream and open the SAP multicast sender.
 * origin_ip is the announce source (0.0.0.0 if unknown). Returns 0 / -1. */
static int sap_announcer_start(struct sap_announcer *sa, const char *name,
                               const char *mcast_ip, uint16_t mcast_port,
                               int pt, int rate, int channels, int ttl,
                               const char *origin_ip, const char *iface_egress)
{
	sa->active = 0;
	char sdp[768];
	struct sdp_params sp = {
		.session_name = name ? name : "REAC", .origin_ip = origin_ip ? origin_ip : "0.0.0.0",
		.mcast_ip = mcast_ip, .mcast_port = mcast_port, .payload_type = pt,
		.rate = rate, .channels = channels, .ttl = ttl,
	};
	if (sdp_build(sdp, sizeof sdp, &sp) < 0)
		return -1;
	/* Hash the rendered SDP (not just the name) so any param change — rate,
	 * channels, mcast — yields a new SAP message-id hash. Per RFC 2974 a
	 * receiver re-parses an announcement only when the hash changes, so a
	 * name-only hash would pin the first-seen SDP forever (stale entries in
	 * PipeWire module-rtp-sap et al.); hashing the body makes discovery dynamic. */
	sa->len = sap_build(sa->pkt, sizeof sa->pkt, sp.origin_ip,
	                    sap_hash(sdp), sdp);
	if (sa->len < 0)
		return -1;
	if (aes67_send_open(&sa->snd, SAP_MCAST_ADDR, SAP_PORT, ttl, iface_egress) != 0)
		return -1;
	sa->active = 1;
	return 0;
}

static void sap_announce(struct sap_announcer *sa)
{
	if (sa && sa->active)
		aes67_send_packet(&sa->snd, sa->pkt, (size_t)sa->len);
}

static void sap_announcer_stop(struct sap_announcer *sa)
{
	if (sa && sa->active) { aes67_send_close(&sa->snd); sa->active = 0; }
}

struct counters { int total, concealed; };
static void emit_count(void *ctx, const uint8_t *buf, size_t len, int concealed)
{
	(void)buf; (void)len;
	struct counters *c = ctx;
	c->total++;
	if (concealed) c->concealed++;
}

/* ---- --dump-samples (decode-only) ---- */
static void dump_samples(const char *path, const struct reac_mode *mode)
{
	struct pcap_source ps;
	if (pcap_source_open(&ps, path) != 0) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
	uint8_t buf[2048], pcm[REAC_MAX_CHANNELS * 24 * 3];
	long n; int frame = 0;
	while ((n = pcap_source_next(&ps, buf, sizeof buf, NULL)) > 0) {
		int ns = reac_decode(buf, (size_t)n, mode, pcm);
		if (ns < 0) { printf("frame %d: MALFORMED\n", frame++); continue; }
		struct reac_frame f = reac_frame_inspect(buf, (size_t)n, mode);
		printf("frame %d seq %u:", frame, f.counter);
		int total = mode->n_channels * ns;
		for (int i = 0; i < total; i++) {
			const uint8_t *p = pcm + (size_t)i * 3;
			int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
			if (v & 0x800000) v |= ~0xffffff;
			printf(" %d", v);
		}
		printf("\n");
		frame++;
	}
	pcap_source_close(&ps);
}

/* ---- pcap replay through the full pipeline (to UDP or to counters) ---- */
static int run_pcap(const char *path, const struct reac_mode *mode,
                    pipeline_emit_fn emit, void *ctx,
                    uint8_t pt, uint32_t ssrc)
{
	struct pipeline pl;
	pipeline_init(&pl, mode, pt, ssrc);
	struct pcap_source ps;
	if (pcap_source_open(&ps, path) != 0) { fprintf(stderr, "cannot open %s\n", path); return 1; }
	uint8_t buf[2048];
	long n;
	while ((n = pcap_source_next(&ps, buf, sizeof buf, NULL)) > 0)
		pipeline_on_frame(&pl, buf, (size_t)n, emit, ctx);
	pcap_source_close(&ps);
	return 0;
}

/* uloop callbacks for run_listen (see struct listen_ctx defined inline there). */
struct listen_ctx { struct pipeline *pl; struct aes67_sender *snd;
	const struct reac_mode *mode; struct reac_capture *cap; struct ubus_stats *us;
	struct sap_announcer *sap; };
static unsigned long g_listen_pkts = 0;
static int g_bcast_only = 0;

/* REAC downstream (the master's program mix) is broadcast; a box's upstream
 * (its stagebox inputs) is unicast back to the master. When both directions
 * share the capture interface (e.g. a bridge VLAN that trunks lan + tunnel),
 * decoding them together interleaves two independent counter streams into
 * concealment garbage. --bcast-only keeps just the broadcast (downstream). */
static inline int frame_wanted(const uint8_t *b, long n)
{
	if (!g_bcast_only) return 1;
	if (n < 6) return 0;
	return (b[0] & b[1] & b[2] & b[3] & b[4] & b[5]) == 0xff; /* dst broadcast */
}

/* capture fd is readable: drain ALL currently-ready frames (non-blocking),
 * feeding each through the pipeline, then return to the event loop. */
static void listen_on_readable(void *ctx)
{
	struct listen_ctx *lc = ctx;
	uint8_t buf[2048];
	long n;
	while ((n = reac_capture_next(lc->cap, buf, sizeof buf)) > 0) {
		if (!frame_wanted(buf, n)) continue;
		pipeline_on_frame(lc->pl, buf, (size_t)n, emit_udp, lc->snd);
		g_listen_pkts++;
	}
	/* n == 0: no more frames ready (EAGAIN); n < 0: error — let the loop run on */
}

/* periodic timer: snapshot pipeline stats into the ubus object + re-announce SAP. */
static void listen_on_publish(void *ctx)
{
	struct listen_ctx *lc = ctx;
	int pps = lc->mode->sample_rate / lc->mode->samples_per_pkt;
	struct pipeline_stats s = pipeline_get_stats(lc->pl);
	ubus_stats_update(lc->us, &s, pps, (unsigned)(g_listen_pkts / (pps ? pps : 1)));
	sap_announce(lc->sap); /* AES67 discovery: re-announce this stream */
}

/* ---- live capture through the full pipeline to UDP ---- */
static int run_listen(const char *ifname, const struct reac_mode *mode,
                      struct aes67_sender *snd, uint8_t pt, uint32_t ssrc,
                      const char *name, const char *udp, const char *ssrc_hex,
                      int plc_xfade, int plc_fade, int ttl, int do_sap,
                      const char *origin, const char *iface_egress)
{
	struct reac_capture cap;
	if (reac_capture_open(&cap, ifname) != 0) {
		fprintf(stderr, "cannot open capture on %s (need CAP_NET_RAW?)\n", ifname);
		return 1;
	}
	struct pipeline pl;
	pipeline_init_ex(&pl, mode, pt, ssrc, plc_xfade, plc_fade);
	struct ubus_stats *us = ubus_stats_start(name ? name : ifname, ifname,
	                                          mode->sample_rate, mode->n_channels,
	                                          udp ? udp : "", ssrc_hex ? ssrc_hex : "");

	/* SAP announcer (AES67 discovery): build from this stream's params. */
	struct sap_announcer sap = {0};
	if (do_sap && udp) {
		char ip[64]; uint16_t port;
		if (parse_ipport(udp, ip, sizeof ip, &port) == 0) {
			if (sap_announcer_start(&sap, name, ip, port, pt, mode->sample_rate,
			                        mode->n_channels, ttl, origin, iface_egress) == 0)
				sap_announce(&sap); /* announce once immediately */
			else
				fprintf(stderr, "SAP announce disabled (setup failed)\n");
		}
	}

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	/* Shared state for the uloop callbacks (and the fallback loop). */
	struct listen_ctx lc = { .pl = &pl, .snd = snd, .mode = mode,
	                         .cap = &cap, .us = us, .sap = &sap };

	struct ubus_stats_loop_cb cb = {
		.on_capture_readable = listen_on_readable,
		.on_publish = listen_on_publish,
		.ctx = &lc,
	};

	/* HAVE_UBUS build: run the uloop (services ubus + capture fd + publish timer
	 * concurrently, so `ubus call` is answered even when REAC is idle). The
	 * capture socket is non-blocking so the readable callback can drain all
	 * ready frames and return. Returns -1 when not compiled with libubus/uloop. */
	reac_capture_set_nonblock(&cap, 1);
	if (ubus_stats_run(us, cap.fd, &cb, 1) != 0) {
		/* Fallback (no ubus/uloop): simple blocking capture loop with a periodic
		 * SAP re-announce (recv times out every ~1s so we re-announce then). */
		reac_capture_set_nonblock(&cap, 0);
		uint8_t buf[2048];
		unsigned long fc = 0;
		const int pps = mode->samples_per_pkt ? mode->sample_rate / mode->samples_per_pkt : 0;
		while (!g_stop) {
			long n = reac_capture_next(&cap, buf, sizeof buf);
			if (n < 0) break;
			if (n == 0) { sap_announce(&sap); continue; } /* EINTR/idle: re-announce + re-check g_stop */
			if (!frame_wanted(buf, n)) continue;
			pipeline_on_frame(&pl, buf, (size_t)n, emit_udp, snd);
			/* Continuous REAC never idles, so the n==0 re-announce above never
			 * fires under live traffic; re-announce ~1/s here too, or discovery
			 * receivers (module-rtp-sap) see only the one startup announce and
			 * eventually expire the session. */
			if (pps > 0 && (++fc % (unsigned long)pps) == 0)
				sap_announce(&sap);
		}
	}
	sap_announcer_stop(&sap);
	ubus_stats_stop(us);
	reac_capture_close(&cap);
	return 0;
}

/* ---- synthetic 40-channel tone stream, real-time paced (demo) ---- */
static int run_gen_tone(const struct reac_mode *mode, struct aes67_sender *snd,
                        uint8_t pt, uint32_t ssrc, int freq, int detune,
                        struct sap_announcer *sap)
{
	struct tone tn;
	tone_init(&tn, freq, 6000000 /* ~ -3 dBFS */, detune);
	struct rtp_params rtp = { .payload_type = pt, .seq = 0, .timestamp = 0, .ssrc = ssrc };
	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	const int ns = mode->samples_per_pkt;
	const long pkt_ns = 1000000000L / (mode->sample_rate / ns); /* 250us @48k */
	const int pps = mode->sample_rate / ns;
	uint8_t pcm[REAC_MAX_CHANNELS * 24 * 3];
	uint8_t pkt[RTP_HEADER_LEN + REAC_MAX_CHANNELS * 24 * 3];

	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	unsigned long count = 0;
	if (sap) sap_announce(sap); /* announce once at start */
	fprintf(stderr, "gen-tone: %d ch @ %d Hz base (+%d/ch), %d Hz pkt-rate; Ctrl-C to stop\n",
	        mode->n_channels, freq, detune, pps);

	while (!g_stop) {
		tone_fill(&tn, pcm, mode);
		long n = rtp_l24_build(pkt, sizeof pkt, &rtp, pcm, mode);
		if (n > 0) aes67_send_packet(snd, pkt, (size_t)n);
		rtp.seq++;
		rtp.timestamp += (uint32_t)ns;
		count++;
		if (sap && pps > 0 && (count % (unsigned long)pps) == 0)
			sap_announce(sap); /* re-announce ~once per second */

		/* pace: sleep until the next packet's deadline (absolute, drift-free) */
		next.tv_nsec += pkt_ns;
		while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	fprintf(stderr, "gen-tone: stopped after %lu packets\n", count);
	return 0;
}

static int parse_ipport(const char *s, char *ip, size_t ipcap, uint16_t *port)
{
	const char *colon = strrchr(s, ':');
	if (!colon) return -1;
	size_t hl = (size_t)(colon - s);
	if (hl == 0 || hl >= ipcap) return -1;
	memcpy(ip, s, hl); ip[hl] = 0;
	int p = atoi(colon + 1);
	if (p <= 0 || p > 65535) return -1;
	*port = (uint16_t)p;
	return 0;
}

/* ---- Dante-interop AES67 profile ----
 * 40 ch -> N <=8-ch multicast flows, 1 ms packets (4 REAC frames aggregated),
 * RTP timestamps anchored to CLOCK_TAI (RFC 7273 mediaclk:direct=0), per-flow
 * RFC 7273 SDP announced via SAP to 239.255.255.255:9875. The live PTP lock
 * (ptp4l, slave-only) + Dante Controller acceptance are validated on hardware;
 * the aggregation/flow-split/SDP/timestamp logic is exercised here. */

static void emit_noop(void *ctx, const uint8_t *b, size_t l, int c)
{ (void)ctx; (void)b; (void)l; (void)c; }

/* base "a.b.c.d" + add (to the last octet) -> out. No octet carry: returns -1 if
 * the last octet would exceed 255 (pick an --aes67-base with room for N flows). */
static int ipv4_add_octet(const char *base, int add, char *out, size_t cap)
{
	unsigned a, b, c, d;
	if (sscanf(base, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
		return -1;
	if (a > 255 || b > 255 || c > 255 || d > 255)
		return -1;
	unsigned last = d + (unsigned)add;
	if (last > 255)
		return -1;
	snprintf(out, cap, "%u.%u.%u.%u", a, b, c, last);
	return 0;
}

struct dante_emit_ctx { struct aes67_sender *senders; int n_flows; int count; };
static void dante_emit(void *ctx, int flow_index, const uint8_t *buf, size_t len,
                       int concealed)
{
	(void)concealed;
	struct dante_emit_ctx *d = ctx;
	d->count++;
	if (d->senders && flow_index < d->n_flows)
		aes67_send_packet(&d->senders[flow_index], buf, len);
}

struct dante_sink_ctx { struct packetizer *pk; packetizer_emit_fn emit; void *emit_ctx; };
static void dante_pcm_sink(void *ctx, const uint8_t *planar, uint32_t rtp_ts,
                           int concealed)
{
	struct dante_sink_ctx *s = ctx;
	packetizer_push(s->pk, planar, rtp_ts, concealed, s->emit, s->emit_ctx);
}

/* Re-send every flow's prebuilt SAP announcement (no-op if SAP is off). */
static void dante_sap_announce(int sap_ok, struct aes67_sender *sap_snd,
                               uint8_t (*sap_pkts)[1024], const long *sap_lens,
                               int n_flows)
{
	if (!sap_ok)
		return;
	for (int i = 0; i < n_flows; i++)
		if (sap_lens[i] > 0)
			aes67_send_packet(sap_snd, sap_pkts[i], (size_t)sap_lens[i]);
}

struct dante_cfg {
	const char *base_ip; uint16_t rtp_port; int ch_per_flow;
	uint8_t pt; uint32_t ssrc_base; int ttl; const char *iface_egress;
	const char *name; const char *origin; const char *gmid; int ptp_domain;
	int media_dscp; int count_only; int do_sap;
};

static int run_dante(const char *pcap, const char *iface,
                     const struct reac_mode *mode, const struct dante_cfg *cfg)
{
	struct reac_flow flows[REAC_MAX_CHANNELS];
	int n_flows = dante_build_flows(flows, REAC_MAX_CHANNELS, mode->n_channels,
	                                cfg->ch_per_flow, cfg->pt, cfg->ssrc_base);
	if (n_flows < 0) {
		fprintf(stderr, "too many Dante flows (%d ch / %d per flow > %d)\n",
		        mode->n_channels, cfg->ch_per_flow, REAC_MAX_CHANNELS);
		return 2;
	}

	/* RTP timestamp anchor from the PTP-disciplined clock (RFC 7273). */
	uint32_t anchor = 0;
	uint64_t tsec; uint32_t tnsec;
	if (ptp_now_tai(&tsec, &tnsec) == 0)
		anchor = tai_to_rtp_ts(tsec, tnsec, mode->sample_rate);
	else
		fprintf(stderr, "warning: CLOCK_TAI unavailable; RTP timestamps not "
		                "PTP-anchored (validate ptp4l on the rig)\n");
	char gmstr[80];
	snprintf(gmstr, sizeof gmstr, "%s:%d", cfg->gmid, cfg->ptp_domain);
	if (!strcmp(cfg->gmid, "00-00-00-00-00-00-00-00"))
		fprintf(stderr, "warning: --gmid is the placeholder; set it to the real "
		                "PTPv2 grandmaster clockId (pmc -> GET CLOCK_DESCRIPTION) "
		                "for Dante to confirm the shared clock\n");

	/* per-flow senders (send mode) */
	struct aes67_sender senders[REAC_MAX_CHANNELS];
	int opened = 0;
	if (!cfg->count_only) {
		for (int i = 0; i < n_flows; i++) {
			char fip[64];
			if (ipv4_add_octet(cfg->base_ip, i, fip, sizeof fip) != 0 ||
			    aes67_send_open(&senders[i], fip, cfg->rtp_port, cfg->ttl,
			                    cfg->iface_egress) != 0) {
				fprintf(stderr, "cannot open Dante flow %d from base %s\n",
				        i, cfg->base_ip);
				for (int j = 0; j < opened; j++) aes67_send_close(&senders[j]);
				return 1;
			}
			aes67_send_set_dscp(&senders[i], cfg->media_dscp); /* AES67 media = AF41 */
			opened++;
		}
	}

	/* per-flow SAP announcements (RFC 7273 SDP) to the Dante SAP group */
	struct aes67_sender sap_snd; int sap_ok = 0;
	uint8_t sap_pkts[REAC_MAX_CHANNELS][1024]; long sap_lens[REAC_MAX_CHANNELS] = {0};
	if (!cfg->count_only && cfg->do_sap) {
		if (aes67_send_open(&sap_snd, SAP_MCAST_ADDR_DANTE, SAP_PORT, cfg->ttl,
		                    cfg->iface_egress) == 0) {
			sap_ok = 1;
			for (int i = 0; i < n_flows; i++) {
				char fip[64]; char nm[64]; char sdp[768];
				if (ipv4_add_octet(cfg->base_ip, i, fip, sizeof fip) != 0)
					continue;
				snprintf(nm, sizeof nm, "%s-%d", cfg->name ? cfg->name : "REAC", i + 1);
				struct sdp_params sp = {
					.session_name = nm, .origin_ip = cfg->origin, .mcast_ip = fip,
					.mcast_port = cfg->rtp_port, .payload_type = flows[i].payload_type,
					.rate = mode->sample_rate, .channels = flows[i].n_ch,
					.ttl = cfg->ttl, .ptime_ms = 1, .ts_refclk = gmstr,
					.mediaclk_direct = 0,
				};
				if (sdp_build(sdp, sizeof sdp, &sp) >= 0)
					sap_lens[i] = sap_build(sap_pkts[i], sizeof sap_pkts[i],
					                        cfg->origin, sap_hash(sdp), sdp);
			}
		} else {
			fprintf(stderr, "Dante SAP announce disabled (setup failed)\n");
		}
	}

	struct packetizer pk;
	packetizer_init(&pk, mode, 4 /* 1 ms @ 48k = 4 REAC frames */, flows, n_flows);
	struct dante_emit_ctx ectx = { .senders = cfg->count_only ? NULL : senders,
	                               .n_flows = n_flows, .count = 0 };
	struct dante_sink_ctx sctx = { .pk = &pk, .emit = dante_emit, .emit_ctx = &ectx };
	struct pipeline pl;
	pipeline_init(&pl, mode, cfg->pt, cfg->ssrc_base);
	pipeline_anchor(&pl, anchor);
	pipeline_set_pcm_sink(&pl, dante_pcm_sink, &sctx);

	dante_sap_announce(sap_ok, &sap_snd, sap_pkts, sap_lens, n_flows);

	int rc = 0;
	if (pcap) {
		struct pcap_source ps;
		if (pcap_source_open(&ps, pcap) != 0) {
			fprintf(stderr, "cannot open %s\n", pcap); rc = 1;
		} else {
			uint8_t buf[2048]; long n;
			while ((n = pcap_source_next(&ps, buf, sizeof buf, NULL)) > 0)
				pipeline_on_frame(&pl, buf, (size_t)n, emit_noop, NULL);
			pcap_source_close(&ps);
		}
	} else if (iface) {
		struct reac_capture cap;
		if (reac_capture_open(&cap, iface) != 0) {
			fprintf(stderr, "cannot open capture on %s (need CAP_NET_RAW?)\n", iface);
			rc = 1;
		} else {
			signal(SIGINT, on_sigint); signal(SIGTERM, on_sigint);
			reac_capture_set_nonblock(&cap, 0);
			int pps = mode->sample_rate / mode->samples_per_pkt; /* ~4000 @48k */
			unsigned long frames = 0;
			uint8_t buf[2048]; long n;
			while (!g_stop) {
				n = reac_capture_next(&cap, buf, sizeof buf);
				if (n < 0) break;
				if (n == 0) { /* idle (EINTR): re-announce SAP */
					dante_sap_announce(sap_ok, &sap_snd, sap_pkts, sap_lens, n_flows);
					continue;
				}
				pipeline_on_frame(&pl, buf, (size_t)n, emit_noop, NULL);
				/* keep the Dante session fresh under continuous audio (~1/s) */
				if (pps > 0 && ++frames % (unsigned long)pps == 0)
					dante_sap_announce(sap_ok, &sap_snd, sap_pkts, sap_lens, n_flows);
			}
			reac_capture_close(&cap);
		}
	}

	if (cfg->count_only)
		printf("rtp_packets %d flows %d\n", ectx.count, n_flows);

	if (sap_ok) aes67_send_close(&sap_snd);
	for (int j = 0; j < opened; j++) aes67_send_close(&senders[j]);
	return rc;
}

int main(int argc, char **argv)
{
	const char *pcap = NULL, *iface = NULL, *udp = NULL, *name = NULL;
	const char *origin = "0.0.0.0";
	int rate = 48000, do_dump = 0, do_count = 0, do_sdp = 0, ttl = 1;
	int plc_xfade = 0, plc_fade = 0, do_sap = 1; /* SAP discovery on by default */
	int gen_tone = 0, tone_freq = 440, tone_detune = 0;
	const char *iface_egress = NULL; /* pin multicast egress (e.g. br-lan) */
	uint8_t pt = 97;
	uint32_t ssrc = 0x11223344;
	/* Dante-interop AES67 profile */
	int profile_dante = 0, pt_set = 0, ptp_domain = 0, ch_per_flow = 8;
	int media_dscp = 34;                          /* AES67 media class AF41 */
	const char *aes67_base = "239.69.10.0";       /* base mcast for the flows */
	const char *gmid = "00-00-00-00-00-00-00-00"; /* PTPv2 grandmaster clockId */
	uint16_t rtp_port = 5004;                     /* AES67 default RTP port */

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pcap") && i + 1 < argc) pcap = argv[++i];
		else if (!strcmp(argv[i], "--listen") && i + 1 < argc) iface = argv[++i];
		else if (!strcmp(argv[i], "--udp") && i + 1 < argc) udp = argv[++i];
		else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pt") && i + 1 < argc) { pt = (uint8_t)atoi(argv[++i]); pt_set = 1; }
		else if (!strcmp(argv[i], "--ssrc") && i + 1 < argc) ssrc = (uint32_t)strtoul(argv[++i], NULL, 16);
		else if (!strcmp(argv[i], "--ttl") && i + 1 < argc) ttl = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
		else if (!strcmp(argv[i], "--origin") && i + 1 < argc) origin = argv[++i];
		else if (!strcmp(argv[i], "--plc-xfade") && i + 1 < argc) plc_xfade = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--plc-fade") && i + 1 < argc) plc_fade = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--gen-tone")) gen_tone = 1;
		else if (!strcmp(argv[i], "--tone-freq") && i + 1 < argc) tone_freq = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--tone-detune") && i + 1 < argc) tone_detune = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-sap")) do_sap = 0;
		else if (!strcmp(argv[i], "--sap")) do_sap = 1;
		else if (!strcmp(argv[i], "--bcast-only")) g_bcast_only = 1;
		else if (!strcmp(argv[i], "--iface") && i + 1 < argc) iface_egress = argv[++i];
		else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile_dante = !strcmp(argv[++i], "dante");
		else if (!strcmp(argv[i], "--dante")) profile_dante = 1;
		else if (!strcmp(argv[i], "--aes67-base") && i + 1 < argc) aes67_base = argv[++i];
		else if (!strcmp(argv[i], "--rtp-port") && i + 1 < argc) rtp_port = (uint16_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ptp-domain") && i + 1 < argc) ptp_domain = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--gmid") && i + 1 < argc) gmid = argv[++i];
		else if (!strcmp(argv[i], "--ch-per-flow") && i + 1 < argc) ch_per_flow = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--media-dscp") && i + 1 < argc) media_dscp = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--dump-samples")) do_dump = 1;
		else if (!strcmp(argv[i], "--count-rtp")) do_count = 1;
		else if (!strcmp(argv[i], "--print-sdp")) do_sdp = 1;
		else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
	}

	if (profile_dante) {
		if (rate != 48000) {
			fprintf(stderr, "note: Dante AES67 interop is 48 kHz only; forcing --rate 48000\n");
			rate = 48000;
		}
		if (!pt_set) pt = 96; /* Dante commonly uses dynamic PT 96 */
	}
	const struct reac_mode *mode = mode_for(rate);

	if (do_sdp && profile_dante) {
		struct reac_flow flows[REAC_MAX_CHANNELS];
		int nf = dante_build_flows(flows, REAC_MAX_CHANNELS, mode->n_channels,
		                           ch_per_flow, pt, ssrc);
		if (nf < 0) { fprintf(stderr, "too many flows\n"); return 2; }
		char gmstr[80]; snprintf(gmstr, sizeof gmstr, "%s:%d", gmid, ptp_domain);
		for (int i = 0; i < nf; i++) {
			char fip[64], nm[64], buf[1024];
			if (ipv4_add_octet(aes67_base, i, fip, sizeof fip) != 0) {
				fprintf(stderr, "bad --aes67-base %s (flow %d overflows)\n", aes67_base, i);
				return 2;
			}
			snprintf(nm, sizeof nm, "%s-%d", name ? name : "REAC", i + 1);
			struct sdp_params sp = {
				.session_name = nm, .origin_ip = origin, .mcast_ip = fip,
				.mcast_port = rtp_port, .payload_type = flows[i].payload_type,
				.rate = mode->sample_rate, .channels = flows[i].n_ch, .ttl = ttl,
				.ptime_ms = 1, .ts_refclk = gmstr, .mediaclk_direct = 0,
			};
			if (sdp_build(buf, sizeof buf, &sp) < 0) { fprintf(stderr, "sdp build failed\n"); return 1; }
			printf("# flow %d  %s  ch %d-%d\r\n", i, fip,
			       flows[i].ch_start, flows[i].ch_start + flows[i].n_ch - 1);
			fputs(buf, stdout);
			printf("\r\n");
		}
		return 0;
	}

	if (do_sdp) {
		char ip[64]; uint16_t port;
		if (!udp || parse_ipport(udp, ip, sizeof ip, &port) != 0) {
			fprintf(stderr, "--print-sdp needs --udp IP:PORT\n"); return 2;
		}
		struct sdp_params sp = {
			.session_name = name ? name : "REAC",
			.origin_ip = origin, .mcast_ip = ip, .mcast_port = port,
			.payload_type = pt, .rate = mode->sample_rate,
			.channels = mode->n_channels, .ttl = ttl,
		};
		char buf[1024];
		if (sdp_build(buf, sizeof buf, &sp) < 0) { fprintf(stderr, "sdp build failed\n"); return 1; }
		fputs(buf, stdout);
		return 0;
	}
	if (do_dump) {
		if (!pcap) { fprintf(stderr, "--dump-samples needs --pcap\n"); return 2; }
		dump_samples(pcap, mode);
		return 0;
	}
	if (do_count) {
		if (!pcap) { fprintf(stderr, "--count-rtp needs --pcap\n"); return 2; }
		if (profile_dante) {
			struct dante_cfg cfg = {
				.base_ip = aes67_base, .rtp_port = rtp_port, .ch_per_flow = ch_per_flow,
				.pt = pt, .ssrc_base = ssrc, .ttl = ttl, .iface_egress = iface_egress,
				.name = name, .origin = origin, .gmid = gmid, .ptp_domain = ptp_domain,
				.media_dscp = media_dscp, .count_only = 1, .do_sap = 0,
			};
			return run_dante(pcap, NULL, mode, &cfg);
		}
		struct counters c = {0, 0};
		int rc = run_pcap(pcap, mode, emit_count, &c, pt, ssrc);
		printf("rtp_packets %d concealed %d\n", c.total, c.concealed);
		return rc;
	}

	/* Dante-interop send path (no --udp; flows derive from --aes67-base). */
	if (profile_dante) {
		if (!pcap && !iface) {
			fprintf(stderr, "--profile dante needs --pcap FILE or --listen IFACE\n");
			return 2;
		}
		struct dante_cfg cfg = {
			.base_ip = aes67_base, .rtp_port = rtp_port, .ch_per_flow = ch_per_flow,
			.pt = pt, .ssrc_base = ssrc, .ttl = ttl, .iface_egress = iface_egress,
			.name = name, .origin = origin, .gmid = gmid, .ptp_domain = ptp_domain,
			.media_dscp = media_dscp, .count_only = 0, .do_sap = do_sap,
		};
		return run_dante(pcap, iface, mode, &cfg);
	}

	/* sending modes need --udp */
	if (udp) {
		char ip[64]; uint16_t port;
		if (parse_ipport(udp, ip, sizeof ip, &port) != 0) {
			fprintf(stderr, "bad --udp IP:PORT: %s\n", udp);
			return 2;
		}
		struct aes67_sender snd;
		if (aes67_send_open(&snd, ip, port, ttl, iface_egress) != 0) {
			fprintf(stderr, "cannot open UDP to %s:%u\n", ip, port);
			return 1;
		}
		aes67_send_set_dscp(&snd, media_dscp); /* AES67 media = AF41 */
		int rc;
		if (gen_tone) {
			struct sap_announcer sap = {0};
			if (do_sap && sap_announcer_start(&sap, name, ip, port, pt,
			        mode->sample_rate, mode->n_channels, ttl, origin, iface_egress) != 0)
				fprintf(stderr, "SAP announce disabled (setup failed)\n");
			rc = run_gen_tone(mode, &snd, pt, ssrc, tone_freq, tone_detune,
			                  sap.active ? &sap : NULL);
			sap_announcer_stop(&sap);
		}
		else if (iface) {
			char ssrc_hex[16];
			snprintf(ssrc_hex, sizeof ssrc_hex, "%08x", ssrc);
			rc = run_listen(iface, mode, &snd, pt, ssrc, name, udp, ssrc_hex,
			                plc_xfade, plc_fade, ttl, do_sap, origin, iface_egress);
		}
		else if (pcap) rc = run_pcap(pcap, mode, emit_udp, &snd, pt, ssrc);
		else { fprintf(stderr, "--udp needs --gen-tone, --listen IF, or --pcap FILE\n"); aes67_send_close(&snd); return 2; }
		aes67_send_close(&snd);
		return rc;
	}

	fprintf(stderr,
	    "usage:\n"
	    "  reac-aes67 --pcap F --dump-samples\n"
	    "  reac-aes67 --pcap F --count-rtp\n"
	    "  reac-aes67 --print-sdp --udp IP:PORT [--name N --rate R --pt N --ttl N --origin IP]\n"
	    "  reac-aes67 --pcap F --udp IP:PORT [--rate R --pt N --ssrc HEX --ttl N]\n"
	    "  reac-aes67 --listen IFACE --udp IP:PORT [--name N --rate R --plc-xfade W --plc-fade P ...]\n"
	    "  reac-aes67 --profile dante (--listen IFACE | --pcap FILE) [--aes67-base 239.69.10.0\n"
	    "             --rtp-port 5004 --ch-per-flow 8 --gmid <eui64> --ptp-domain 0 --iface br-lan]\n"
	    "  reac-aes67 --print-sdp --profile dante  (prints the per-flow Dante SDPs)\n"
	    "\nAES67 egress goes to whichever network owns the route to the multicast\n"
	    "group in --udp; put that group on a separate bridge/VLAN/SSID from REAC.\n"
	    "--profile dante: 48 kHz, 1 ms packets, <=8-ch flows, RFC 7273 SDP, SAP to\n"
	    "239.255.255.255:9875; needs the bridge PTPv2-slaved to the Dante grandmaster.\n");
	return 2;
}
