# reac-aes67 — Architecture

`reac-aes67` bridges a Roland **REAC** feed (audio-over-Ethernet,
EtherType `0x8819`) to **AES67** RTP/L24 multicast. It captures REAC
frames on a network interface, decodes the multichannel 24-bit audio,
re-clocks and conceals losses, and re-emits each packet as a
standards-based RTP stream that any AES67 receiver — PipeWire in
particular — can subscribe to. The decode/clock/PLC/RTP core is
socket-free and dependency-free (`-lm` only), so it builds the same on a
dev host, in a container, and cross-compiled onto an OpenWrt 25.12 router.

## Audio flow

```
REAC frame (0x8819)
  → reac_capture     raw AF_PACKET capture (or pcap_source for replay)
  → reac_decode      validate frame, de-interleave to planar signed-24-bit-LE
  → media_clock      assign RTP sample-count timestamp; detect loss from l2_counter
  → plc              gap-fill: crossfade → burst-fade → hold-last → silence
  → rtp_l24_build    RFC 3550 header + re-interleave to big-endian L24 payload
  → aes67_send       UDP/multicast egress (one sendto per datagram)
  → sap / sdp        periodic SAP announce carrying the SDP description
```

`pipeline` is the socket-free heart that ties decode → media_clock →
PLC → rtp_l24 together. `reac_capture` feeds it frames; `aes67_send`
transmits what it produces. When the media clock reports N lost packets
before a received one, the pipeline emits N concealment packets first
(each its own RTP packet and timestamp), then the real packet — so a
single drop never slips every later sample out of channel alignment.

## Units

- **reac_mode** — the stream descriptor (`sample_rate`, `n_channels`,
  `samples_per_pkt`) and the wire constants. Holds `REAC_MODE_48K` and
  `REAC_MODE_96K`.
- **reac_capture** — live raw capture via `AF_PACKET`/`SOCK_RAW`, bound
  to an interface, filtering EtherType `0x8819` in-loop. Needs `CAP_NET_RAW`.
- **pcap_source** — same frame stream from a classic libpcap file, for
  off-hardware replay and CI. Drives the identical pipeline as live capture.
- **reac_decode** — pure validate + de-interleave of one 1492-byte frame
  to planar signed-24-bit-LE PCM (channel-major). No I/O.
- **media_clock** — per-stream RTP timestamping. Timestamp is a running
  sample count; advances by `samples_per_pkt` per packet, and by
  `N*samples_per_pkt` across an `l2_counter` gap so the timeline stays aligned.
- **plc** — packet-loss concealment over the planar PCM, search-free and
  constant-time per channel: repeat + crossfade, then decaying burst-fade,
  then hold-last, then silence as a last resort.
- **rtp_l24** — builds one RTP L24 packet: 12-byte RFC 3550 header plus
  audio re-interleaved frame-major and flipped little-endian → big-endian.
  Socket-free, fully unit-testable.
- **aes67_send** — thin UDP socket shell: opens to a multicast group (or
  unicast) and sends pre-built datagrams; sets multicast TTL/interface and
  AF41 DSCP.
- **sdp** — renders the RFC 4566 / AES67 session description (`L24`,
  `RTP/AVP`, `a=recvonly`), with optional RFC 7273 clock lines.
- **sap** — wraps the SDP in an RFC 2974 SAP announce and multicasts it so
  receivers auto-create the audio node with no per-receiver config.
- **packetizer** — aggregates frames and splits channels into flows for the
  Dante profile (see below). Bypassed on the plain AES67 path.
- **ptp_clock** — TAI/PTP helpers for the Dante profile: derives the
  RTP timestamp from the PTP-disciplined `CLOCK_TAI`.

## Clock model

REAC is **rate-invariant**. Every frame is a fixed 1492 bytes — 50 B L2
header + 1440 B audio (40 channels × 12 samples × 3 B) + 2 B end marker
(`0xC2 0xEA`) — at every sample rate. The rate is carried by the **packet
rate**, not by repacking the frame:

```
rate = pps × 12
  44.1 kHz → 3675 pps   (272 µs slot)
  48   kHz → 4000 pps   (250 µs slot)   verified on-site
  96   kHz → 8000 pps   (125 µs slot)   verified on-rig 2026-06-06
```

96 kHz keeps all 40 channels and doubles the packet rate; it does **not**
halve channels (the earlier `{96000, 20, 24}` guess was refuted by the
measured ~8000 pps / 40-channel capture). The mixer/console is the single
clock master; the bridge only **observes** that clock, it never generates
it. The converter does not auto-detect the rate — it is set explicitly via
config (`--rate 48000` / `--rate 96000`); any non-`96000` value falls back
to 48 kHz mode.

On the egress side, the RTP timestamp is a sample count, not wall-clock.
By default it free-runs from 0 (plain AES67). Under the Dante profile it is
anchored to a TAI-derived value so two senders on the same PTP grandmaster
stay sample-phase aligned.

## Dante-interop profile

The default AES67 path emits one RTP packet per REAC frame (250 µs / 12
samples) with a free-running clock. Dante's AES67 interop needs a tighter
profile, handled by `packetizer` + `ptp_clock`:

- **1 ms packets** — 4 REAC frames (48 samples) aggregated per RTP packet
  (`frames_per_packet = 4`).
- **≤8-channel flows** — the 40 channels are split into contiguous flows,
  each with its own SSRC, sequence, and multicast destination.
- **PTP-locked clock** — RTP timestamp derived from `CLOCK_TAI`
  (`a=mediaclk:direct=0`, RFC 7273) instead of free-running, so the stream
  is genlocked to the shared PTPv2 grandmaster (ptp4l, slave-only, on the
  router).
- **SDP clock lines** — `a=ts-refclk` / `a=mediaclk` announce the grandmaster
  and lock mode; AES67 media is marked AF41 (DSCP 34) on egress.

## References

- [FreeREAC/reac-protocol](https://github.com/FreeREAC/reac-protocol) —
  the REAC wire-format reference: frame layout, per-rate packetization,
  EtherType, and end marker.
- [`../OVERVIEW.md`](../OVERVIEW.md) — what it is, the problem it solves, and
  the verified protocol facts.
- [FreeREAC/reac-lab](https://github.com/FreeREAC/reac-lab) — raw captures,
  on-rig journals, and the deep design specs this doc replaces.
