# reac-aes67 — Worked Examples & Recipes

Each recipe is a self-contained scenario: a **goal**, the **exact commands**
(with generic placeholders), and the **expected result**. The daemon bridges
Roland REAC (EtherType `0x8819`) into AES67 (RTP L24 multicast) for receivers
such as PipeWire.

> **Placeholder convention.** Substitute your own values:
>
> | Placeholder      | Meaning                                     | Example value      |
> |------------------|---------------------------------------------|--------------------|
> | `<REAC_IFACE>`   | NIC where REAC arrives (raw capture)        | `lan1`             |
> | `<MCAST_GROUP>`  | AES67 destination multicast group           | `239.69.0.1`       |
> | `<PORT>`         | AES67 RTP UDP port                          | `5004`             |
> | `<NAME>`         | Stream / SDP session name                   | `REAC-A`           |
> | `<ORIGIN_IP>`    | SDP origin IP (`o=` line)                   | `10.0.0.1`         |
> | `<CAPTURE>`      | A classic libpcap file of a REAC stream     | `stream.pcap`      |
>
> All paths below are repo-relative (`src/...`, `demo/...`). Build the binary
> first with `make` (output `build/reac-aes67`).

---

## Core facts that shape every recipe

These constraints are not optional knobs — they follow from how REAC works on
the wire:

- **REAC is rate-invariant; the rate is set by the packet rate.** Every sample
  rate uses the same fixed `REAC_FRAME_BYTES` = 1492-byte frame carrying
  40 channels × 12 samples/packet × 3 B = 1440 B of audio. The sample rate is
  set by the *packet rate*, not the frame: pps = rate / 12, so 44.1 kHz = 3675
  pps, 48 kHz = 4000 pps, 96 kHz = 8000 pps. 96 kHz keeps all 40 channels and
  doubles the packet rate. You must declare the rate with `--rate`. `--rate
  48000` → `REAC_MODE_48K` = `{48000, 40, 12}`; `--rate 96000` → `REAC_MODE_96K`
  = `{96000, 40, 12}`. Any `--rate` value other than `96000` falls back to
  48 kHz mode (`mode_for()` in `src/main.c`).
- **96 kHz is verified on-rig: 40 channels at 8000 pps.** The 96 kHz stream was
  measured on the rig at ~8000 pps carrying 40 channels (double-pps, not
  channel-halving). The earlier channel-halving model (20 channels / 24 samples)
  is rejected. See the wire-format reference at
  <https://github.com/FreeREAC/reac-protocol/blob/main/wire-format.md>.
- **AES67 egress is route-table-driven only.** `aes67_send_open()` in
  `src/aes67_send.c` opens a plain `SOCK_DGRAM` socket and sets only
  `IP_MULTICAST_TTL` and `IP_MULTICAST_LOOP=1`. It does **not** set
  `IP_MULTICAST_IF` or bind to a source interface. Which NIC the RTP leaves on
  is decided entirely by the kernel routing table for the multicast group in
  `--udp`. The daemon's own usage footer states it:
  *"AES67 egress goes to whichever network owns the route to the multicast
  group in --udp; put that group on a separate bridge/VLAN/SSID from REAC."*
- **`IP_MULTICAST_LOOP` is forced on.** A receiver on the *same host* (e.g. a
  local PipeWire demo) always sees the stream; this is intended and harmless
  off-host.
- **TTL is clamped to a minimum of 1** (`--ttl 0` becomes `1`).
- **Live capture needs `CAP_NET_RAW`.** `--listen` uses an `AF_PACKET`
  `SOCK_RAW` socket. Run as root, with the capability granted, or (on OpenWrt)
  via the packaged procd capabilities file.
- **No real-time pacing on replay.** Only `--gen-tone` paces output (drift-free
  `clock_nanosleep` at ~250 µs/packet). `--pcap` replay floods frames as fast as
  the file is read.

---

## (a) Single REAC zone → one AES67 stream + a local PipeWire receiver

**Goal.** Capture one live REAC zone on `<REAC_IFACE>` and emit it as a single
AES67 L24 multicast stream that a PipeWire receiver on the network (or the same
host) can subscribe to.

**Commands.**

Start the bridge (live capture → AES67). `--listen` requires `CAP_NET_RAW`, so
run it privileged:

```sh
sudo build/reac-aes67 --listen <REAC_IFACE> --udp <MCAST_GROUP>:<PORT> \
  --rate 48000 --name <NAME> --pt 97 --ssrc 11223344 --ttl 1
```

On the receiving host, drop in the persistent PipeWire receiver
(`demo/reac-a-rtp-source.conf`) and restart PipeWire:

```sh
mkdir -p ~/.config/pipewire/pipewire.conf.d
cp demo/reac-a-rtp-source.conf ~/.config/pipewire/pipewire.conf.d/
systemctl --user restart pipewire
```

The shipped conf matches the demo's example values (group `239.69.0.1`, port
`5004`, `audio.format = S24BE`, `audio.rate = 48000`, `audio.channels = 40`,
`node.name = reac-a-rtp`), which in turn line up with the daemon's non-network
defaults (`--rate 48000`, `--pt 97`, `L24/48000/40`). Note: `--udp` has **no**
default — the group and port must always be passed explicitly. If you used a
different `<MCAST_GROUP>`/`<PORT>`, edit `source.ip` / `source.port` in the conf
to match before restarting.

Verify the source node appeared:

```sh
wpctl status | grep -i reac
```

**Expected result.**

- The daemon runs continuously, emitting ~4000 RTP L24 packets/s. Each 48 kHz
  packet is 1452 bytes (12-byte RFC 3550 header + 40 × 12 × 3 = 1440-byte
  payload), with `V=2`, `PT=97`, big-endian sequence/timestamp/SSRC, RTP
  timestamp advancing by 12 samples per packet.
- A 40-channel `Audio/Source` node named `REAC-A (AES67)` (node name
  `reac-a-rtp`) appears in PipeWire (`wpctl`, `qpwgraph`).
- On `--listen`, the daemon also publishes ubus stats (on OpenWrt builds) and
  installs `SIGINT`/`SIGTERM` handlers for clean shutdown.

> **Note.** The `--plc-xfade` and `--plc-fade` PLC-tuning flags are only wired
> into `--listen` mode (via `pipeline_init_ex`). They are accepted by the arg
> parser globally but silently ignored in `--pcap`, `--count-rtp`, and
> `--gen-tone`.

---

## (b) Three zones (A/B/C) → three streams on a dedicated AES67 network

**Goal.** Bridge three independent REAC zones, each captured on its own
interface, into three separate AES67 streams, with all AES67 traffic leaving on
a dedicated network segment that is isolated from the REAC clock domain.

**Network design (no daemon change).** REAC is the timing-critical clock domain
(~48 Mbit/s broadcast per zone at 48 kHz, more at 96 kHz) and is isolated on its
own VLANs/ports. AES67 multicast must **not** share those segments. Put the
AES67 multicast subnet on a dedicated `aes67` bridge; the kernel then routes each
stream's RTP out that interface because the group lives on its subnet. From
`openwrt/files/network-aes67.example`, appended to `/etc/config/network`:

```
config interface 'aes67'
	option proto 'static'
	option type 'bridge'
	option ipaddr '10.67.0.1'
	option netmask '255.255.255.0'
	# list ports 'lan5'   # optional: reserve a wired AES67 port
```

> **IMPORTANT.** Do **not** bridge `aes67` with the REAC capture interfaces.
> REAC is captured raw (`AF_PACKET`) on the capture NICs; AES67 is produced on
> `aes67`. They are different L2 domains by design. Separation is achieved
> purely by putting each stream's group inside the `aes67` subnet — no flag on
> the daemon selects the egress NIC.

**Commands.** Choose three groups inside the AES67 subnet (e.g.
`239.69.0.0/24`) so egress lands on the `aes67` segment. One daemon instance per
zone:

```sh
sudo build/reac-aes67 --listen <REAC_IFACE_A> --udp 239.69.0.1:5004 --rate 48000 --name REAC-A --ssrc 11223344 --ttl 1
sudo build/reac-aes67 --listen <REAC_IFACE_B> --udp 239.69.0.2:5004 --rate 48000 --name REAC-B --ssrc 11223345 --ttl 1
sudo build/reac-aes67 --listen <REAC_IFACE_C> --udp 239.69.0.3:5004 --rate 48000 --name REAC-C --ssrc 11223346 --ttl 1
```

On OpenWrt the same thing is declarative: one `config stream` UCI section per
zone in `/etc/config/reac-aes67`, each with its own `iface`, `mcast_addr`,
`name`, and `ssrc`; the procd init starts one daemon instance per enabled
section. The LuCI app and the example UCI ship three stream slots (`a`/`b`/`c`)
as the suggested layout; this is advisory only — `addremove` is enabled with no
enforced maximum, and `config_foreach start_stream stream` loops over all
enabled sections, so more can be added.

**Expected result.**

- Three continuous streams, each on a distinct group, all egressing the `aes67`
  bridge while REAC capture stays on its isolated interfaces.
- Three receivers (one per group/port) each see a 40-channel `Audio/Source`.
- Distinct SSRCs and names keep the streams individually identifiable on the
  wire and in stats.

> **Tip.** Give each stream a distinct `--ssrc` (parsed as **hex**) so receivers
> and analyzers can tell the streams apart.

---

## (c) No-hardware bench demo (`--gen-tone` + PipeWire receiver)

**Goal.** Prove the full send→receive path with no REAC hardware: generate a
synthetic 40-channel tone, emit it as AES67, and confirm a 40-channel node
appears in PipeWire.

**Commands.** The turnkey script (`demo/demo.sh`) does it end-to-end —
start sender, load a held-open PipeWire RTP receiver, and assert a 40-channel
node is on the graph:

```sh
make
./demo/demo.sh
```

Useful env knobs (defaults shown): `KEEP=1` leaves sender + receiver running
(Ctrl-C to stop), `HOLD=N` holds the `pw-cli` receiver open `N` seconds
(default 8). The group/port are hardcoded in the script to `239.69.0.1:5004`.

```sh
KEEP=1 ./demo/demo.sh
```

To do it manually, run the tone sender (continuous; background + kill it):

```sh
build/reac-aes67 --gen-tone --rate 48000 --udp <MCAST_GROUP>:<PORT> --tone-detune 25 &
```

…then use the persistent receiver conf from recipe (a), or load a transient one:

```sh
pw-cli load-module libpipewire-module-rtp-source \
  source.ip=<MCAST_GROUP> source.port=<PORT> \
  sess.latency.msec=100 audio.format=S24BE audio.rate=48000 audio.channels=40
```

**Expected result.**

- `gen-tone` prints a startup banner to stderr
  (`gen-tone: <n_channels> ch @ <freq> Hz base (+<detune>/ch), <pktrate> Hz pkt-rate; Ctrl-C to stop`)
  and emits a continuous ~4000 pkt/s tone at amplitude 6000000 (~−3 dBFS),
  drift-free. Each channel is detuned by `--tone-detune` Hz so channels are
  distinguishable.
- `demo.sh` prints `DEMO OK: 40-channel AES67 stream visible on the
  network/graph` once a node with `rtp` in its name and ≥40 channels is found
  via `pw-dump`.
- `--gen-tone` has no built-in stop: in scripts it must be backgrounded and
  killed. A SIGTERM exit (143, e.g. from `timeout`) is a normal/healthy stop,
  not a crash.

> **Note.** `--gen-tone` is **not** listed in the program's own printed usage
> block, but it is fully supported and reachable via the `--udp` branch. It
> requires `--udp`, and it takes precedence over both `--listen` and `--pcap` in
> source selection.

---

## (d) Print / inspect an SDP for a receiver

**Goal.** Produce an RFC 4566 / AES67 SDP description that a receiver can import
to subscribe to a stream. This opens no socket and processes no audio.

**Commands.**

```sh
build/reac-aes67 --print-sdp --udp <MCAST_GROUP>:<PORT> \
  --name <NAME> --rate 48000 --pt 97 --ttl 1 --origin <ORIGIN_IP>
```

Save it to a file for a receiver that consumes SDP:

```sh
build/reac-aes67 --print-sdp --udp <MCAST_GROUP>:<PORT> --name <NAME> --rate 48000 > <NAME>.sdp
```

**Expected result.** An SDP block on stdout (exact line shapes, with `<...>`
filled from your flags):

```
v=0
o=- 1 1 IN IP4 <ORIGIN_IP>
s=<NAME>
c=IN IP4 <MCAST_GROUP>/<TTL>
t=0 0
m=audio <PORT> RTP/AVP <PT>
a=rtpmap:<PT> L24/<RATE>/<CHANNELS>
a=ptime:1
a=recvonly
```

For 48 kHz this gives `a=rtpmap:97 L24/48000/40`; with `--rate 96000` it becomes
`L24/96000/40` (96 kHz keeps all 40 channels). Line endings are CRLF.

> **Gotchas.**
> - `--print-sdp` requires `--udp IP:PORT`; without it the daemon prints
>   `--print-sdp needs --udp IP:PORT` and exits 2.
> - Defaults if omitted: `--name REAC`, `--origin 0.0.0.0`, `--pt 97`,
>   `--ttl 1`, `--rate 48000`.
> - The connection line shows `<MCAST_GROUP>/<TTL>` — the TTL from `--ttl`, not
>   a count.

---

## (e) Offline analysis of a capture (replay, count, dump, cross-verify)

**Goal.** Analyze a recorded REAC capture (`<CAPTURE>`, a classic libpcap file)
without any hardware: replay it to AES67, count the RTP packets it would
produce, dump decoded samples, and cross-verify the C decoder against an
independent Python reference.

The `--pcap` source reads classic libpcap files only (magic `0xA1B2C3D4` native
or `0xD4C3B2A1` swapped) and yields full Ethernet frames, so the same decode
path runs as in live capture.

### e.1 — Replay a capture to an AES67 receiver

```sh
build/reac-aes67 --pcap <CAPTURE> --udp <MCAST_GROUP>:<PORT> \
  --rate 48000 --pt 97 --ssrc 11223344 --ttl 1
```

**Expected result.** Frames are pushed through the full
decode → media-clock → PLC → RTP-L24 → AES67 pipeline. **No real-time pacing** —
the file is replayed as fast as it reads, so the receiver sees a burst, not a
real-time stream. Useful for feeding a receiver under test, not for timing.

### e.2 — Count RTP packets (and concealed packets)

```sh
build/reac-aes67 --pcap <CAPTURE> --count-rtp
```

**Expected result.** One line: `rtp_packets <total> concealed <concealed>`.
`concealed` counts packets the PLC flagged as concealment (emitted for REAC
sequence-counter gaps). For a clean capture, `concealed 0`. `--count-rtp`
**requires** `--pcap` (else `--count-rtp needs --pcap`, exit 2).

### e.3 — Dump decoded samples (decode-only)

```sh
build/reac-aes67 --pcap <CAPTURE> --dump-samples
```

**Expected result.** One line per frame:
`frame N seq U: <s0> <s1> ...` — 24-bit signed samples sign-extended from the
little-endian 3-byte PCM, `n_channels * samples_per_pkt` samples per frame. This
path does **not** touch RTP/UDP/the media clock. `--dump-samples` **requires**
`--pcap` (else `--dump-samples needs --pcap`, exit 2).

### e.4 — Cross-verify the C decoder against the Python reference

```sh
make verify
```

…which runs:

```sh
python3 tests/cross_verify.py build/reac-aes67 <CAPTURE>
```

The capture is resolved from `REAC_FIXTURE` (default fallback in `Makefile`).
Point it at your own file by exporting it:

```sh
REAC_FIXTURE=<CAPTURE> make verify
```

**Expected result.** `cross_verify.py` runs `build/reac-aes67 --dump-samples`
and compares it sample-for-sample against an independent Python re-implementation
of the obs-h8819 `convert_to_pcm24lep` de-interleave (which reads the pcap bytes
directly and does **not** call the C code). It exits 0 with a line like
`OK: <F> frames, <N> samples - C decoder matches Python reference exactly`, or
nonzero with a diff on mismatch.

> **Gotchas.**
> - `cross_verify.py` hardcodes `n_ch=40, ns=12`. That per-packet geometry holds
>   at both 48 kHz and 96 kHz (96 kHz uses the same 40-channel/12-sample packet,
>   doubling the packet rate to 8000 pps), but the script verifies the de-interleave
>   only — it does not measure or assert the packet rate.
> - The full test suite (`make test`) runs the C tests plus `test_cli_sdp.sh`.

---

## (f) Passively record a live *wired* REAC network (non-intrusive tap)

**Goal.** Record the channels on an existing **wired** mixer↔stagebox REAC link — the
desk's REAC output — **without disturbing it**. This is safe by construction: in
`--listen` mode the daemon opens an `AF_PACKET` `SOCK_RAW` socket and **transmits no
REAC frame** — it does no split handshake, announces nothing, and is invisible to the
desk. The master broadcasts to `ff:ff:ff:ff:ff:ff` and a receiver is "connected" by
frame length alone (no handshake — see the
[wire-format reference](https://github.com/FreeREAC/reac-protocol/blob/main/wire-format.md)),
so a passive tap sees all the audio while staying invisible. The only thing the daemon
sends is the AES67 multicast, on a separate network.

**Get a copy of the stream onto a NIC** — pick the least intrusive tap your rig allows:

- **Desk REAC split / mirror output** (best): patch the recorder's NIC straight into a
  dedicated REAC split port on the desk or stagebox. The live link is never touched.
- **Passive 100-Mbit Ethernet TAP**: an inline tap copies the REAC line to a monitor
  port → recorder. A true passive tap doesn't buffer, so the link passes through
  untouched.
- **Unmanaged 100BASE-TX switch**: mixer → switch → stagebox, a 3rd port → recorder.
  REAC is broadcast, so the switch floods it to the recorder. Use a clean full-duplex
  100M unmanaged switch — REAC is phase-strict, so avoid a store-and-forward unit that
  adds jitter. (A managed switch's mirror/SPAN port works too; mirroring is copy-only.)

What you capture: the **downstream** broadcast (mixer → stagebox) carries whatever the
desk routes to that REAC output — clean, labelled, 40 channels. The upstream return
(stagebox preamps → desk, unicast to the desk's MAC) is also on the wire, but its
channel map is FPGA-scrambled, so record via a desk **REAC output**.

**Keep the two networks apart** (same rule as recipe (b)): run the recorder with two
interfaces — one is the REAC tap (`--listen` reads it, nothing leaves it), the other
carries AES67 to the DAW. Egress is route-table-driven, so putting the `--udp` group on
the AES67 subnet keeps the recording multicast off the REAC wire.

**Commands.**

```sh
# eth0 = the REAC tap (split port / passive TAP / unmanaged-switch port)
sudo build/reac-aes67 --listen eth0 \
  --udp 239.69.0.1:5004 --rate 48000 \      # 48000 or 96000 — MATCH the desk; not auto-detected
  --name REAC-REC --pt 97 --ssrc 11223344 --ttl 1
```

Point the DAW at it via SDP (recipe (d)) or a PipeWire RTP source (recipe (a)). On
OpenWrt the same thing is a `config stream` UCI section with the tap `iface` + the AES67
group, with AES67 on a dedicated `aes67` bridge (recipe (b)).

**Expected result.** A 40-channel AES67 `L24/<rate>/40` stream the DAW records, with the
wired mixer↔stagebox link completely unaffected — the desk never sees the recorder.

> **Caveats.**
> - `--rate` must match the desk (48 k or 96 k); a wrong rate decodes as garbage.
> - Don't insert a jittery switch into the *live* link — prefer the desk split output or
>   a passive TAP. The recorder's own link quality doesn't matter; only the
>   mixer↔stagebox path does.
> - 96 kHz REAC nearly fills 100BASE-TX — don't add load to that wire.

---

## Quick reference — modes & precedence

Mode is chosen by precedence in `main()`, not a single `--mode` flag. Order:
`--print-sdp` → `--dump-samples` → `--count-rtp` → the `--udp` sending branch.
Inside `--udp`, the source is selected as `--gen-tone` → `--listen` → `--pcap`.
So a flag can be silently shadowed (e.g. `--print-sdp` wins over
`--pcap --dump-samples`).

| Recipe | Invocation skeleton |
|--------|---------------------|
| (a) live → AES67 | `--listen <REAC_IFACE> --udp <MCAST_GROUP>:<PORT> [--rate R --name N --pt N --ssrc HEX --ttl N --plc-xfade W --plc-fade P]` |
| (b) N zones | one `--listen` per zone, distinct `--udp` group + `--ssrc` (or one UCI `config stream` per zone on OpenWrt) |
| (c) bench demo | `--gen-tone --udp <MCAST_GROUP>:<PORT> [--rate R --pt N --ssrc HEX --ttl N --tone-freq N --tone-detune N]` |
| (d) SDP | `--print-sdp --udp <MCAST_GROUP>:<PORT> [--name N --rate R --pt N --ttl N --origin IP]` |
| (e) replay | `--pcap <CAPTURE> --udp <MCAST_GROUP>:<PORT> [--rate R --pt N --ssrc HEX --ttl N]` |
| (e) count | `--pcap <CAPTURE> --count-rtp` |
| (e) dump | `--pcap <CAPTURE> --dump-samples` |
| (e) verify | `make verify` / `python3 tests/cross_verify.py build/reac-aes67 <CAPTURE>` |
| (f) passive record | `--listen <REAC_TAP_IFACE> --udp <MCAST_GROUP>:<PORT> --rate R` — same flags as (a); tap a live wired link non-intrusively via a desk split port / passive TAP / unmanaged switch |

**CLI defaults:** `--rate 48000`, `--pt 97`, `--ssrc 11223344` (hex),
`--ttl 1`, `--name REAC`, `--origin 0.0.0.0`, `--tone-freq 440`,
`--tone-detune 0`. `--udp` has **no** default and is mandatory for every
sending/SDP mode.
