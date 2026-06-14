# reac-pw

PipeWire-native Roland REAC endpoint. A `libpipewire-0.3` client that registers
the REAC fabric as nodes in the PipeWire graph instead of a fixed point-to-point
bridge. Once REAC is a graph node, every destination is a `pw-link` away: monitor
on a DAC, emit AES67 through `module-rtp-sink`, or record to a file — same node,
no new code per destination.

## What it is

This first cut ships the **RX source node**: a 40-channel `reac:capture`
Audio/Source fed from a live REAC wire (AF_PACKET, EtherType `0x8819`) or a pcap
replay, decoded with the proven plain-LE core and handed to PipeWire's adapter
for channel-map, format-convert and adaptive resample. The **TX sink node**
(`reac:playback`, the virtual stagebox) is registered as an interface-only
skeleton; the libreac TX layer it needs is unbuilt (see Status).

Everything REAC-specific is reused, not reinvented:

- **libreac** (`FreeREAC/libreac`, header `<reac/reac.h>`) — frame validate, the
  byte-14/15 counter, gap math, `reac_detect_rate_fd` / `reac_rate_snap`. Pulled
  as a meson subproject.
- **reac-aes67 core** (`FreeREAC/reac-aes67`, `src/`) — `reac_decode.c` (plain-LE
  sample-major `(s*40+ch)*3`, on-rig coherence 0.999), `reac_capture.c` (live
  AF_PACKET), `pcap_source.c` (classic pcap reader). Compiled straight in from a
  sibling checkout.

reac-pw itself is only the lock-free ring, the RX feeder, and the two PipeWire
nodes.

## Build and run

External deps are just PipeWire and SPA via pkg-config (Fedora: `pipewire-devel`),
plus pthreads and libm. libreac is fetched by the meson wrap; the reac-aes67 core
is read from a sibling checkout (`../reac-aes67-pub` by default — override with
`-Dreac_aes67=PATH`).

```
meson setup   build
meson compile -C build
meson test    -C build                              # ring unit test, no PipeWire needed
./build/reac-pw --pcap capture.pcap --rate 48000    # offline replay
sudo ./build/reac-pw --live reac0                   # live wire (needs CAP_NET_RAW)
```

`--rate` forces 44100/48000/96000; omit it to auto-detect from packet cadence on
a live wire. `--tx IFNAME` registers the inert sink skeleton for graph visibility.

## Node model

The REAC broadcast is always 40 ch × 12 samples × 3 B; the sample rate lives in
the packet rate (pps = rate/12), never on the wire.

- **`reac:capture` (source).** A `pw_filter` with 40 mono-F32 DSP output ports —
  exactly the ring's planar layout. A non-realtime feeder thread reads frames,
  validates and counter-stamps with libreac, decodes with the reac-aes67 core,
  and writes whole REAC frames into a lock-free SPSC ring. The only realtime code
  is `on_process()`: it dequeues one PipeWire quantum per channel and returns —
  no format or rate conversion, that's the adapter on each outgoing link.
  Underrun zero-fills (the DAC never gets garbage); steady drift is the
  resampler's job. The node defaults to a **follower**: the DAC/NIC clock drives
  the graph and PipeWire async-resamples REAC↔graph, with the feeder publishing a
  filtered ppm error (counter slope vs `CLOCK_MONOTONIC`) into the port's
  `SPA_IO_RateMatch`. Setting it as the graph **driver** instead runs REAC as the
  master clock and async-resamples the DAC to it. Same node, only the driver flag
  + clock registration differ.
- **`reac:playback` (sink).** Mirrors the source so it fills in symmetrically once
  a TX layer lands, but emits no wire traffic today.

See [DESIGN.md](DESIGN.md) for the data path, the two clock topologies, and the
TX contract. This realizes `NATIVE-REAC-DESIGN.md` §3.4 (REAC as pw-filter nodes,
adaptive resample via `io_rate_match`).

## Status

- **RX source node** — implemented (pcap + live), follower clock with ppm
  tracking; offline-testable.
- **Lock-free ring** — implemented, unit-tested (round-trip, underrun, overrun);
  the test needs no PipeWire so CI can run it anywhere.
- **TX sink node** — interface-only skeleton, inert. libreac is RX/measure-only
  (no frame builder, checksum-apply, interleaver or emitter), so there is nothing
  to call yet. The runtime pieces still to build — TX ring, a SCHED_FIFO slot
  pacer, and the JOIN/HOLD connection FSM — are out of scope for this cut.

Target: Fedora + PipeWire 1.4.

GPL-3.0-or-later. Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>.
