# Configuration Reference

`reac-aes67` decodes Roland REAC (EtherType `0x8819`) captured raw off an
OpenWrt router and re-emits each stream as an AES67 RTP L24 multicast flow.
On OpenWrt the daemon is driven by UCI; the `procd` init script translates UCI
options into the daemon's lower-level command-line flags and runs one daemon
instance per enabled stream. This document covers both layers and the
network-separation model that keeps the REAC clock domain isolated from the
AES67 egress.

---

## 1. UCI configuration — `/etc/config/reac-aes67`

This file is the single source of truth on OpenWrt. The `procd` init script
(`openwrt/files/reac-aes67.init`) spawns one daemon instance per enabled
`stream` section; runtime state is published over ubus, not written back here.
The shipped template is `openwrt/files/reac-aes67.config`.

### 1.1 Global section

```
config reac-aes67 'global'
	option enabled '1'
```

| Option | Type | Default | Meaning |
| --- | --- | --- | --- |
| `enabled` | bool | `1` | Master on/off gate. The init script reads it with `config_get_bool genabled global enabled 1`; if it is not `1`, `start_service` logs `globally disabled` and starts nothing, regardless of per-stream `enabled` flags. |

There is exactly one `global` section.

### 1.2 Stream section

Each REAC stream to bridge gets its own `config stream '<id>'` section. The
template ships one enabled stream (`'a'`) plus two commented-out examples
(`'b'`, `'c'`); the LuCI app presents three stream slots as the suggested
layout. There is no enforced maximum — the procd init starts one instance per
enabled section and the LuCI form's `addremove` is enabled, so more sections
can be added.

```
config stream 'a'
	option enabled '1'
	option name 'REAC-A'
	option iface '<REAC_IFACE>'        # e.g. lan1
	option rate '48000'
	option mcast_addr '<MCAST_GROUP>'  # e.g. 239.69.0.1
	option mcast_port '<PORT>'         # e.g. 5004
	option payload_type '97'
	option ssrc '11223344'
	option ttl '1'
	# option plc_xfade '0'             # 0 = default crossfade width
	# option plc_fade_pkts '0'         # 0 = default burst fade length
```

Every option, with the validator type/default the init script enforces via
`uci_validate_section` and the daemon flag it maps to:

| Option | Validator (type:default) | Default | Daemon flag | Meaning |
| --- | --- | --- | --- | --- |
| `enabled` | `bool:1` | `1` | (none) | Per-stream on/off. The init script skips the instance unless this is `1`. |
| `name` | `string` | (falls back to section id) | `--name` | Stream/session name. Used as the SDP `s=` session name, the ubus object suffix (`reac-aes67.<name>`), and the LuCI Status row label. If empty, the init script passes `--name <section-id>`. |
| `iface` | `string` | (required) | `--listen` | Capture interface for live AF_PACKET REAC ingress (e.g. `<REAC_IFACE>` such as `lan1`). An empty `iface` makes the init script log `stream <id> has no iface` and skip the instance. |
| `rate` | `or("48000","96000"):48000` | `48000` | `--rate` | REAC sample rate. **Must** be set to match the REAC clock master; it is not auto-detected (see §4). Both `48000` and `96000` carry 40 channels / 12 samples per packet; `96000` runs the same partitioning at twice the packet rate (8000 pps vs 4000 pps). |
| `mcast_addr` | `host:` (none) | (required) | first half of `--udp` | AES67 RTP destination IPv4 address (typically a multicast group like `<MCAST_GROUP>`). An empty `mcast_addr` makes the init script log `stream <id> has no mcast_addr` and skip. |
| `mcast_port` | `port:5004` | `5004` | second half of `--udp` | AES67 RTP destination UDP port (1..65535). |
| `payload_type` | `range(96,127):97` | `97` | `--pt` | RTP payload type. Constrained to the dynamic range 96..127. |
| `ssrc` | `string:11223344` | `11223344` | `--ssrc` | RTP SSRC, written in hex (the daemon parses `--ssrc` base-16). |
| `ttl` | `range(1,255):1` | `1` | `--ttl` | Multicast TTL (1..255). The daemon additionally clamps any value below 1 up to 1. |
| `plc_xfade` | `uinteger:0` | `0` | `--plc-xfade` (only when > 0) | Packet-loss-concealment crossfade window, in samples. `0` keeps the daemon default. The init script appends `--plc-xfade <n>` only when the value is greater than 0. |
| `plc_fade_pkts` | `uinteger:0` | `0` | `--plc-fade` (only when > 0) | PLC burst-fade length, in packets. `0` keeps the daemon default. The init script appends `--plc-fade <n>` only when the value is greater than 0. |

If `uci_validate_section` rejects a stream, the init script logs
`stream <id> failed validation, skipping` and that instance is not started;
other valid streams still run.

### 1.3 How the init script builds the command

For each enabled, valid stream the init script opens a procd instance named
`reac-<section-id>` and runs:

```
/usr/bin/reac-aes67 \
	--listen <iface> \
	--udp <mcast_addr>:<mcast_port> \
	--rate <rate> \
	--pt <payload_type> \
	--ssrc <ssrc> \
	--ttl <ttl> \
	--name <name|section-id>
```

with `--plc-xfade <n>` and/or `--plc-fade <n>` appended only when those options
are greater than 0. The instance is configured with:

- `procd_set_param respawn 3600 5 0`
- `procd_set_param stderr 1`
- `procd_set_param capabilities /etc/capabilities/reac-aes67.json`

The capabilities file grants only `net_raw`, which is what raw AF_PACKET
capture requires; the daemon does not run as root. A reload trigger is
registered via `procd_add_reload_trigger reac-aes67`, so editing
`/etc/config/reac-aes67` and reloading restarts the affected instances.

---

## 2. Two-network AES67 separation model

REAC is the timing-critical clock domain and is bandwidth-heavy (broadcast
traffic on the order of tens of Mbit/s per zone at 48 kHz, double that at
96 kHz since the packet rate doubles).
AES67 multicast (a few Mbit/s per stream) must **not** share the REAC L2
segments. The recommended topology isolates REAC capture interfaces on their
own VLANs/ports and gives AES67 a dedicated egress network.

This separation needs **no daemon change**. The daemon sends each stream's RTP
to the address in its `mcast_addr`/`--udp`; the kernel routing table decides
which interface that traffic leaves on. The daemon deliberately does not pin an
egress interface (see §3.4), so placing the AES67 multicast subnet on a
dedicated bridge is sufficient to steer egress there.

### 2.1 The `aes67` bridge (`openwrt/files/network-aes67.example`)

Append a dedicated bridge interface to `/etc/config/network`. It owns the
AES67 multicast subnet, so normal routing sends each stream's RTP out this
interface only:

```
config interface 'aes67'
	option proto 'static'
	option type 'bridge'
	option ipaddr '<AES67_ROUTER_IP>'     # e.g. 10.67.0.1
	option netmask '255.255.255.0'
	# Reserve a physical port for a wired AES67 receiver/uplink (optional):
	# list ports '<AES67_PORT>'            # e.g. lan5
```

If routing does not already prefer this interface for the group, pin it with an
explicit route (commented in the example):

```
config route
	option interface 'aes67'
	option target '<AES67_MCAST_SUBNET>'  # e.g. 239.69.0.0
	option netmask '255.255.255.0'
```

The `aes67` bridge can carry the AES67 receivers over a wired port (via
`list ports`), a dedicated Wi-Fi SSID, or both.

### 2.2 Optional dedicated Wi-Fi SSID

A `wifi-iface` bridged onto `aes67` lets receivers subscribe over Wi-Fi
(`/etc/config/wireless`, commented in the example):

```
config wifi-iface 'aes67_ap'
	option device 'radio0'
	option network 'aes67'
	option mode 'ap'
	option ssid '<AES67_SSID>'
	option encryption 'sae'
	option key '<AES67_PSK>'
```

### 2.3 Tying it together

Set each stream's `mcast_addr` to a group inside the AES67 subnet (for example
`<MCAST_GROUP>` within `239.69.0.0/24`) so its egress lands on the `aes67`
segment.

**Do not bridge `aes67` with the REAC capture interfaces.** REAC is captured
raw (AF_PACKET) on the capture interfaces; AES67 is produced on `aes67`. These
are different L2 domains by design.

---

## 3. CLI flag reference (the lower-level interface)

`/usr/bin/reac-aes67` is what the procd init drives. It is a flat hand-rolled
argv loop (no `getopt`, no `--help`); any unrecognized token prints
`unknown arg: <x>` and exits 2. Running with no arguments (and no `--udp`)
prints the usage block and exits 2.

The mode is chosen by precedence in `main()`, not by a single `--mode` flag:
`--print-sdp` is checked first, then `--dump-samples`, then `--count-rtp`, then
the `--udp` sending branch. Inside the sending branch the source sub-mode is
`--gen-tone` (wins), else `--listen`, else `--pcap`.

### 3.1 Live capture (what procd runs)

```
reac-aes67 --listen <REAC_IFACE> --udp <MCAST_GROUP>:<PORT> \
           [--name N --rate R --pt N --ssrc HEX --ttl N --plc-xfade W --plc-fade P]
```

Opens an AF_PACKET raw capture on the interface (needs `CAP_NET_RAW`; on
failure prints `cannot open capture on <if> (need CAP_NET_RAW?)`), starts a
ubus stats object, installs `SIGINT`/`SIGTERM` handlers, then loops
capture → pipeline → AES67 send. It publishes ubus stats roughly once per
second (`sample_rate / samples_per_pkt` is 4000 pkt/s at 48 kHz and 8000 pkt/s
at 96 kHz, since both carry 12 samples per packet). This is the only mode that
wires
`--plc-xfade`/`--plc-fade` into the pipeline (via `pipeline_init_ex`).

### 3.2 Offline replay (test / off-site)

```
reac-aes67 --pcap <FILE> --udp <MCAST_GROUP>:<PORT> [--rate R --pt N --ssrc HEX --ttl N]
```

Reads a libpcap file and pushes frames through the full pipeline to AES67.
**There is no real-time pacing on replay** — frames are emitted as fast as the
file is read. `--plc-xfade`/`--plc-fade` are accepted by the parser but ignored
here (this path calls `pipeline_init`, not `pipeline_init_ex`).

### 3.3 Inspection / no-network modes

```
reac-aes67 --pcap <FILE> --dump-samples
reac-aes67 --pcap <FILE> --count-rtp
```

- `--dump-samples` requires `--pcap` (else `--dump-samples needs --pcap`,
  exit 2). Decodes and prints one line per frame:
  `frame N seq U: <s0> <s1> ...` of sign-extended 24-bit samples. It does not
  touch RTP/UDP/clock.
- `--count-rtp` requires `--pcap` (else `--count-rtp needs --pcap`, exit 2).
  Runs the full pipeline and prints `rtp_packets <total> concealed <n>`.

### 3.4 SDP emission (no socket, no audio)

```
reac-aes67 --print-sdp --udp <MCAST_GROUP>:<PORT> [--name N --rate R --pt N --ttl N --origin IP]
```

Requires `--udp IP:PORT` (else `--print-sdp needs --udp IP:PORT`, exit 2).
Prints the SDP description to stdout and exits; it opens no socket and processes
no audio. See §5.

### 3.5 Synthetic tone (local demo)

```
reac-aes67 --gen-tone --udp <MCAST_GROUP>:<PORT> [--rate R --pt N --ssrc HEX --ttl N --tone-freq N --tone-detune N]
```

A continuous, full-channel-count sine generator, real-time paced via
drift-free `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` (one packet every
250 µs at 48 kHz and every 125 µs at 96 kHz, since `sample_rate / samples_per_pkt`
is 4000 pkt/s and 8000 pkt/s respectively). Amplitude is fixed at `6000000`
(~ -3 dBFS). It lives inside the `--udp`
branch and takes precedence over `--listen` and `--pcap`. It is **not** listed
in the program's printed usage block, though it is fully supported. As a
continuous sender it has no built-in stop; in scripts it must be backgrounded
and killed.

### 3.6 Common flags

| Flag | Argument | Default | Notes |
| --- | --- | --- | --- |
| `--pcap` | FILE | — | libpcap input path; source for replay/dump/count. |
| `--listen` | IFACE | — | AF_PACKET live-capture interface; needs `CAP_NET_RAW`. |
| `--udp` | IP:PORT | — (required for all sending/SDP modes) | AES67 RTP destination. Split on the **last** `:`; numeric IPv4 only (no hostname resolution); port validated 1..65535. Bad value prints `bad --udp IP:PORT: <s>`, exit 2. |
| `--rate` | 48000\|96000 | `48000` | Selects the REAC packet rate (4000 pps at 48 kHz, 8000 pps at 96 kHz); both carry 40 channels / 12 samples per packet. Any value other than `96000` selects the 48 kHz mode (no error on a typo). See §4. |
| `--pt` | N | `97` | RTP payload type (parsed via `atoi`, cast to `uint8_t`). |
| `--ssrc` | HEX | `11223344` | RTP SSRC, parsed base-16. |
| `--ttl` | N | `1` | Multicast TTL; any value below 1 is clamped to 1. |
| `--name` | N | `REAC` (SDP) / iface (ubus) | SDP session name / ubus object suffix / stream label. |
| `--origin` | IP | `0.0.0.0` | SDP origin IP (`--print-sdp` only). |
| `--plc-xfade` | W | `0` | PLC crossfade window (samples). Only honored in `--listen`. |
| `--plc-fade` | P | `0` | PLC burst-fade length (packets). Only honored in `--listen`. |
| `--gen-tone` | (flag) | off | Synthetic tone mode; requires `--udp`. |
| `--tone-freq` | N | `440` | gen-tone base frequency (Hz). |
| `--tone-detune` | N | `0` | gen-tone per-channel detune (Hz). |
| `--dump-samples` | (flag) | off | Decode-only print mode; requires `--pcap`. |
| `--count-rtp` | (flag) | off | RTP packet counter; requires `--pcap`. |
| `--print-sdp` | (flag) | off | Emit SDP; requires `--udp`. |

There is **no default multicast group or port** — every sending mode and
`--print-sdp` require an explicit `--udp IP:PORT`.

### 3.7 Egress routing — no interface pinning

`aes67_send_open` creates a plain `AF_INET SOCK_DGRAM` socket and sets only
`IP_MULTICAST_TTL` and `IP_MULTICAST_LOOP` (forced to 1). It does **not** set
`IP_MULTICAST_IF` and does **not** bind to a source interface. Which NIC the
AES67 RTP leaves on is therefore decided entirely by the kernel routing table
for the destination group in `--udp`. The usage footer states this:

> AES67 egress goes to whichever network owns the route to the multicast
> group in --udp; put that group on a separate bridge/VLAN/SSID from REAC.

`IP_MULTICAST_LOOP` is forced on so a receiver (e.g. PipeWire) on the same host
sees the stream — needed for the local demo, harmless otherwise. It is not
configurable.

---

## 4. Sample rate must match the REAC clock master (not wire-detected)

REAC is rate-invariant: every sample rate carries 40 channels × 12 samples ×
3 B = 1440 B of audio in the same fixed 1492-byte frame. The sample rate is set
by the **packet rate**, not by the frame layout: `pps = rate / 12`, so 48 kHz =
4000 pps and 96 kHz = 8000 pps. 96 kHz keeps all 40 channels and doubles the
packet rate; it does **not** halve channels. Because the frame layout is
identical at both rates, the sample rate is **not detectable from a single
frame** and must be configured to match the REAC clock master (e.g. a digital
mixer/console acting as the timing source) — the operator sets which
clock-master rate is running.

Operational consequences:

- Set `rate`/`--rate` explicitly. Both `48000` and `96000` select 40 channels /
  12 samples; `96000` just runs at 8000 pps instead of 4000 pps.
- A wrong rate de-interleaves against the wrong packet cadence and gives a
  garbled stream.
- The daemon's `mode_for()` falls back to the 48 kHz mode for any `--rate`
  value other than exactly `96000`, with no validation error.

The 96 kHz model (`REAC_MODE_96K = {96000, 40, 12}`, 8000 pps) is verified
against an on-rig 96 kHz capture that measured ~8000 pps carrying 40 channels.
The full wire-format reference is at
<https://github.com/FreeREAC/reac-protocol/blob/main/wire-format.md>.

---

## 5. SDP / discovery

`--print-sdp` emits an RFC 4566 / AES67 SDP description for the L24 multicast
stream to stdout (it opens no socket and sends no audio). The fields come from
`--name` (session name, default `REAC`), `--origin` (origin IP, default
`0.0.0.0`), the address/port from `--udp`, `--pt`, the rate and channel count
from the selected mode, and `--ttl`. The emitted block has the shape:

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

Notes:

- `a=ptime:1` (1 ms) is a common AES67 default; receivers such as PipeWire
  accept it.
- The `o=` line uses a fixed session id/version of `1` — adequate for a static
  description (but see *stale-origin pitfall* below for the live SAP case).
- `--print-sdp` is the *description generator* (stdout only, no socket). A live
  `--listen` stream additionally **announces** that SDP over SAP — see
  §5.1 — and publishes runtime state over ubus as `reac-aes67.<name>` with a
  single `status` method; the LuCI Status view polls these. ubus is the
  discovery surface for live state; SDP is the static media description.

### 5.1 Being seen by AES67 / Dante gear

A live stream is announced with **SAP** (RFC 2974, on by default; disable with
`--no-sap`). Whether stock third-party gear actually sees the announcement
depends on three things — all already handled by the daemon, but worth
understanding for a deployment:

- **SAP group / dialect.** The plain AES67 path announces to the well-known
  **`224.0.0.56:9875`**, which PipeWire's `module-rtp-sap` scans by default —
  fine Linux-to-Linux. Most stock **AES67/Dante** equipment instead scans the
  AES67 SAP group **`239.255.255.255:9875`** (the `--profile dante` path
  announces there). If a hardware receiver does not discover the stream, the
  group it scans is the first thing to check: PipeWire's default group is not
  the one Dante Controller listens on.
- **Dante needs an RFC 7273 clock.** Dante will not lock an AES67 flow from a
  free-running sender. It requires the **PTP clock declared in the SDP**
  (`a=ts-refclk:ptp=IEEE1588-2008:…` + `a=mediaclk:direct=…`, RFC 7273) **and**
  a stream whose RTP timestamps are actually PTP-locked — i.e. the
  `--profile dante` path (PTP-locked clock + per-flow RFC 7273 SDP), not the
  casual default. Plain Linux/PipeWire receivers do not need this.
- **Stale-origin pitfall.** A SAP receiver pins the **first** SDP it sees for a
  given stream *name* and ignores later changes — so if the stream's parameters
  change, a receiver that already cached the old description keeps using it. The
  daemon avoids this by hashing the rendered SDP into the SAP message id and
  re-deriving it whenever anything changes, so a changed stream looks like a new
  announcement rather than a silently-ignored update. If you script SDP
  generation by hand, do the same: bump the `o=` session version and vary the
  SAP id when the description changes, never re-announce a changed stream under
  the original origin.

Net: Linux/PipeWire receivers discover the stream as soon as it is announced on
the group they scan; AES67/Dante hardware additionally needs the AES67 group
plus the RFC 7273 PTP clock and a PTP-locked stream (the `--profile dante`
path). You can always distribute the `--print-sdp` output out of band and
configure a receiver directly with the group, port, format (`L24`), rate, and
channel count.
