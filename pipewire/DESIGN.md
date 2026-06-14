# reac-pw — PipeWire-native REAC endpoint

A libpipewire-0.3 client that puts a Roland REAC fabric into the PipeWire graph
as first-class nodes. This first cut ships the **RX source node** (the monitor
box): a 40-channel `reac:capture` Audio/Source fed from a live REAC wire or a
pcap replay. The **TX sink node** (`reac:playback`, the virtual stagebox) is
scaffolded as an interface only — the libreac TX layer it needs does not exist
yet.

Realizes NATIVE-REAC-DESIGN.md §3.4 (REAC as pw-filter nodes, adaptive resample
via `io_rate_match`, follower vs driver clock topologies). Target: Fedora +
PipeWire 1.4.

Reuses, does not reinvent:

- **libreac** (`FreeREAC/libreac`, `<reac/reac.h>`) — RX validate, the byte-14/15
  counter, gap math, `reac_detect_rate_fd` / `reac_rate_snap`. Pulled as a meson
  subproject.
- **reac-aes67 core** (`FreeREAC/reac-aes67`, `src/`) — `reac_decode.c` (plain-LE
  sample-major `(s*40+ch)*3`, on-rig coherence 0.999), `reac_capture.c`
  (AF_PACKET 0x8819), `pcap_source.c` (classic pcap reader). Compiled straight in
  from a sibling checkout.

Everything REAC-specific is borrowed. reac-pw itself is only: the lock-free ring,
the RX feeder, and the two PipeWire nodes.

## Data path (RX, Phase 1)

```
REAC NIC (AF_PACKET 0x8819)  ─┐
                              ├─► reac_rx feeder thread (SCHED_OTHER, producer)
pcap replay (offline test)  ─┘     reac_frame_is_reac / reac_frame_counter   [libreac]
                                   reac_decode → planar s24 → f32             [reac-aes67 core]
                                   reac_ring_write
                                        │  (lock-free SPSC ring, planar f32)
                                        ▼
                                   on_process()  (PipeWire RT callback)
                                   reac_ring_read_planar → 40 output ports
                                        │
                                        ▼
                                   PipeWire adapter per link:
                                   channel-map + format-convert + adaptive RESAMPLE
                                        │
                              ┌─────────┼───────────────┐
                              ▼         ▼               ▼
                          ALSA/DAC   module-rtp-sink   file sink
                          (monitor)  (AES67 out)       (record)
```

One node, every destination is a `pw-link` away. AES67 stops being the hub and
becomes one optional output.

## Node model

### Source node `reac:capture` (the monitor box) — `reac_source_node.c`

A `pw_filter` registered `media.class = Audio/Source`, with `REAC_MAX_CHANNELS`
(40) DSP output ports (mono F32 planar — exactly the ring's layout). The node
advertises `node.rate = 1/<recovered REAC rate>` from `reac_detect_rate_fd`.

`on_process()` is the only realtime code: it dequeues one quantum per channel
from the ring into the port buffers and returns. It never converts formats or
rates — PipeWire's adapter on each outgoing link does channel-map,
format-convert (S24→graph F32) and resample.

**Ports / format / rate.** 40 output ports, `32 bit float mono audio` DSP
format, rate = the recovered REAC rate (48000 default, auto-detected on live,
snapped from cadence by libreac). The downstream broadcast is always 40 ch × 12
samp; the rate lives in packet cadence (pps = rate/12), never on the wire.

**RX hot-path → process() via the ring.** The feeder (`reac_rx.c`, a plain
pthread, NOT SCHED_FIFO — it's the producer) reads frames, decodes with the
reac-aes67 core into planar f32, and `reac_ring_write`s whole REAC frames (40 ×
12). `on_process` `reac_ring_read_planar`s one PipeWire quantum. The ring is a
single-producer/single-consumer lock-free SPSC (`reac_ring.c`): two atomics,
no locks, no allocation on the hot path. Underrun → process() zero-fills and
bumps a counter (the DAC never gets garbage); transient overrun → the feeder
drops the oldest frame (steady drift is the resampler's job, not the ring's).
Ring depth ≈ 250 ms at the recovered rate — enough to swallow a WiFi tail spike
(~8 ms) without touching the ~200 ms TX-mute (which the counter free-runs
through; we never resync the media clock to a transmission gap).

**Clock: follower (default, Tier-A) vs driver.** Default the node is a
**follower** (`PW_FILTER_FLAG_RT_PROCESS`, no driver flag): the DAC or NIC PHC
drives the graph and PipeWire async-resamples REAC↔graph. The feeder is the
**rate authority** — it tracks the byte-14/15 counter slope against
`CLOCK_MONOTONIC` and publishes a slowly-filtered ppm error
(`rx->ppm_error_milli`, recomputed ~4×/s, locked to the long-term slope not
packet jitter). The source node feeds that into the port's `SPA_IO_RateMatch`
so PipeWire's resampler tracks the desk's true rate. **That is the Tier-A clock
bridge, PipeWire-provided.** To run REAC as the graph **driver** instead (a pure
monitor sample-locked to the desk, no RX resampling), set `PW_KEY_NODE_DRIVER`
and register a clock source at the recovered rate — the DAC is then
async-resampled *to* REAC. Both topologies are the same node; only the driver
flag + clock registration differ.

### Sink node `reac:playback` (the virtual stagebox) — `reac_sink_node.c`

**Interface only, inert.** Mirrors the source so it fills in symmetrically once
the TX layer lands, but emits no wire traffic today. See the TX note below.

## Build (meson + pkg-config)

`meson.build` + `meson_options.txt`. External deps via pkg-config: only
`libpipewire-0.3` and `libspa-0.2` (Fedora `pipewire-devel`) plus `threads` and
optional `m`. No libpcap (the pcap reader is the dependency-free reac-aes67 one;
live capture is raw AF_PACKET).

- **libreac** is a **meson subproject** (`subprojects/libreac.wrap`, a `wrap-git`
  on `FreeREAC/libreac`). Upstream libreac ships only a hand Makefile, so the
  wrap drops a tiny `meson.build` into the checkout via `patch_directory`
  (`subprojects/packagefiles/libreac/meson.build`) that builds `src/reac.c` into
  a static lib and exposes `libreac_dep` with `<reac/reac.h>`. No upstream source
  is touched.
- **reac-aes67 decode/capture/pcap core** is compiled **straight in** from a
  sibling checkout (`-Dreac_aes67=../reac-aes67`, default `../reac-aes67`),
  exactly as reac-aes67's own Makefile compiles libreac's `reac.c` in. The three
  files are dependency-free and already on-rig-proven; forking them would violate
  "reuse, do not reinvent". meson errors with a clear message if the checkout is
  missing.

```
meson setup build -Dreac_aes67=../reac-aes67-pub
meson compile -C build
meson test    -C build            # runs test_reac_ring (no PipeWire needed)
./build/reac-pw --pcap capture.pcap --rate 48000
sudo ./build/reac-pw --live reac0           # needs CAP_NET_RAW
```

`test_reac_ring` is self-contained (ring only — no PipeWire, no libreac), so CI
can run it anywhere. The node code needs `pipewire-devel` installed to compile.

## TX note (sink scaffolded only)

`reac:playback` is declared but inert. The REAC TX layer it depends on does not
exist: libreac is RX/measure-only (grep-verified — no frame builder, no
checksum-apply, no interleaver, no emitter), so there is nothing to call. The
sink header (`reac_sink_node.h`) documents the contract it will use once libreac
grows a TX layer:

- `reac_tx_build_frame()` — the 1492-B frame builder
- plain-LE sample-major interleave `(s*N+ch)*3`
- `data[31]` modular checksum apply (FILLER exempt)
- free-running u16-LE counter stamp (bytes 14-15)

and the runtime pieces also still to build: a TX ring (symmetric to the RX ring),
a **SCHED_FIFO slot pacer** (the reac_repacer.c pattern — prio ~79, mlockall,
affinity, `clock_nanosleep` TIMER_ABSTIME) firing every slot period
(125.0/250.0/272.1 µs), and the Phase-2 JOIN/HOLD connection FSM. Here the wire
cadence is the rate authority, so the sink is a follower in reverse: PipeWire
resamples the *graph* into our fixed pps. `reac_sink_node_new` returns a
non-NULL skeleton that holds ports for graph visibility and logs a clear "TX
layer unbuilt" warning. None of the TX path is in scope for this first cut.

## Files

| File | Role |
|---|---|
| `src/main.c` | CLI + lifecycle: open feeder, create source node, (optional) scaffold sink, run the loop |
| `src/reac_ring.{h,c}` | lock-free SPSC planar-float ring (RX hot-path → process()) |
| `src/reac_rx.{h,c}` | non-RT feeder: wire source (live/pcap) → libreac validate → reac-aes67 decode → f32 → ring; counter-slope ppm estimator |
| `src/reac_source_node.{h,c}` | `reac:capture` pw_filter: 40 F32 ports, RT process(), follower/driver clock |
| `src/reac_sink_node.{h,c}` | `reac:playback` interface-only scaffold (TX layer unbuilt) |
| `tests/test_reac_ring.c` | SPSC ring unit test (round-trip, underrun, overrun) |
| `meson.build`, `meson_options.txt` | build: pipewire/spa via pkg-config, libreac subproject, reac-aes67 core sibling |
| `subprojects/libreac.wrap` + `packagefiles/libreac/meson.build` | libreac as a meson subproject |
