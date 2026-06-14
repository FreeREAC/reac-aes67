# reac-aes67 — Overview & Use Cases

## What it is

`reac-aes67` is a small C daemon that bridges **Roland REAC**
(audio-over-Ethernet, EtherType `0x8819`) to **AES67** (RTP L24
multicast). It captures REAC frames on a network interface, decodes the
multichannel 24-bit audio, and re-emits each packet as a standards-based
AES67 RTP/L24 stream that a standard Linux audio graph — PipeWire in
particular — can subscribe to as an ordinary network audio source.

The decode/clock/concealment/RTP core is dependency-free (links only
`-lm`), so the same code builds and runs on a developer host, in a
container, or cross-compiled onto an OpenWrt 25.12 router — aarch64
(`mediatek/filogic`) or mips (`ramips/mt7621`) — where it ships as two apk
packages: the daemon (`reac-aes67`) and
a LuCI web app (`luci-app-reac-aes67`).

## The problem it solves

REAC is a proprietary Layer-2 transport: the audio lives in raw Ethernet
frames with a custom EtherType, not in anything a normal audio stack
understands. Getting those channels onto a Linux box has historically
meant either dedicated Roland hardware on the receiving end, or a bespoke
capture program bound to a specific NIC.

This bridge removes both requirements:

- **No dedicated capture NIC on the consumer.** The bridge does the raw
  capture once (via `AF_PACKET` `SOCK_RAW`) and hands off as plain RTP
  multicast. Any number of downstream hosts then receive audio over
  ordinary IP networking — no special interface, no kernel module.
- **No bespoke receiver software.** AES67 RTP L24 is an open standard.
  On the consumer side, PipeWire's stock
  `libpipewire-module-rtp-source` (or any AES67-capable receiver) picks
  the stream up. The bridge also emits a matching **SDP** description so
  receivers can be configured from the advertised parameters.

The result: a REAC feed becomes a normal multichannel `Audio/Source`
node on the Linux audio graph, available to record, monitor, route, or
analyze with standard tools.

## How the audio flows

```
REAC frame (0x8819)
  → reac_decode      validate + de-interleave to planar signed-24-bit-LE
  → media_clock      assign RTP sample-count timestamp; detect packet loss
  → plc              conceal lost packets (crossfade → burst-fade → hold-last → silence)
  → rtp_l24_build    RFC 3550 header + re-interleave to big-endian L24 payload
  → aes67_send       UDP/multicast egress (sendto per datagram)
```

The RTP timestamp is a running **sample count**, not wall-clock time. On
loss, the clock advances by the lost sample span *and* the bridge emits
concealment packets in the gap, so a single dropped packet does not slip
every subsequent sample out of alignment across channels.

## The desk picks the clock — the bridge follows it

REAC has a **single clock master** (typically the V-Mixer console), and on
the desk that master clock is **selectable**: the front panel exposes a
clock-source selector with **word clock**, **REAC A**, **REAC B**,
**internal**, and **AES** options (observed on the console's clock-source
menu; a desk with two REAC ports presents both A and B as candidates). The
bridge has no say in this — it only **observes** whatever clock the desk is
running and recovers the rate from the wire cadence. Two practical
consequences:

- **The chosen source sets the rate the bridge must be told.** Whichever
  source is selected (an external word clock, a slaved REAC port, or the
  internal oscillator) is what drives the fabric word clock, and the bridge
  cannot detect the rate from a single frame (see *Verified facts*). Set
  `--rate` to match the rate the desk is actually clocking at.
- **A bridge or repeater must not fight the desk's clock.** Everything on
  the fabric is genlocked to the master's chosen source; the bridge holds
  output and keeps counting across the master's deliberate transmit gaps
  rather than resyncing to a gap, so it stays phase-aligned with the desk
  instead of inventing its own timing.

## Use cases

### Record or monitor REAC inputs on a Linux host

Run the bridge in live capture mode against the REAC interface and let
PipeWire expose the result as a multichannel source:

```sh
reac-aes67 --listen <REAC_IFACE> --udp <MCAST_GROUP>:<PORT>
```

The receiver sees one `Audio/Source` node carrying all 40 channels at
both 48 kHz and 96 kHz. Capture into any PipeWire-aware recorder, or
monitor channels live.

### Route into a DAW, OBS, or other application

Because the output is a standard PipeWire source, it patches into any
application that consumes PipeWire/JACK nodes — a DAW for multitrack
recording, OBS for streaming/capture, or a routing tool to split
individual channels to different destinations. No application needs to
know anything about REAC.

### Multi-listener on a LAN

AES67 egress is multicast (`m=audio <PORT> RTP/AVP <pt>`), so multiple
hosts on the same network can subscribe to one `<MCAST_GROUP>:<PORT>`
simultaneously without the bridge sending a separate copy per listener.
The bridge can also target a plain unicast address (e.g. `127.0.0.1`)
for a single same-host consumer.

> **Network separation.** AES67 egress goes to whichever network owns the
> route to the multicast group in `--udp`. The socket does **not** pin a
> source interface (`IP_MULTICAST_IF`/`SO_BINDTODEVICE` are not set), so
> placement is decided entirely by the host routing table. Put the AES67
> multicast group on a separate bridge/VLAN/SSID from the REAC capture
> link — REAC is a timing-critical clock domain and should not share L2
> with the AES67 output. On the OpenWrt package this is done by defining a
> dedicated `aes67` bridge interface and choosing an `mcast_addr` inside
> its subnet; no daemon change is required.

### Bench testing with the synthetic tone

For wiring up and validating a receiver without a live REAC source, the
bridge can generate a continuous multichannel test tone with the same
channel count and packet cadence as a real stream. Each channel gets a
distinct frequency so they are individually identifiable:

```sh
reac-aes67 --gen-tone --udp <MCAST_GROUP>:<PORT> [--tone-freq <HZ> --tone-detune <HZ>]
```

The tone is drift-free, absolute-deadline paced (one packet per
~250 µs). It runs until stopped, so in scripts it must be backgrounded
and killed. (Note: `--gen-tone` is valid but is not printed in the
program's own usage block — it lives under the `--udp` branch.)

### Off-line analysis from packet captures

A previously recorded REAC capture (classic libpcap format) can be
replayed through the same decode path used for live capture:

```sh
reac-aes67 --pcap <FILE> --dump-samples     # decode and print samples, one line per frame
reac-aes67 --pcap <FILE> --count-rtp        # run the pipeline, print "rtp_packets N concealed N"
reac-aes67 --pcap <FILE> --udp <MCAST_GROUP>:<PORT>   # replay a capture out as AES67
```

`--dump-samples` and `--count-rtp` both require `--pcap`. Replay is **not**
real-time paced — it pushes frames as fast as the file yields them.

### Inspect the advertised stream description

Print the SDP that describes the AES67 stream (for configuring receivers
or documenting a deployment) without opening a socket or processing audio:

```sh
reac-aes67 --print-sdp --udp <MCAST_GROUP>:<PORT> [--name <NAME> --rate <R> --pt <N> --ttl <N> --origin <IP>]
```

This emits an `L24` `RTP/AVP` session description (`a=rtpmap:<pt>
L24/<rate>/<channels>`, `a=recvonly`).

## Verified facts

| Fact | Value |
|------|-------|
| REAC EtherType | `0x8819` (`#define REAC_ETHERTYPE 0x8819`) |
| Frame size | Fixed **1492 bytes** at every rate (50 B L2 header + 1440 B audio + 2 B end marker `0xC2 0xEA`) |
| Sample resolution | 24-bit (`REAC_RESOLUTION 3` bytes/sample) |
| **48 kHz mode** | **40 channels**, 12 samples/packet (`REAC_MODE_48K = {48000, 40, 12}`) at 4000 pps — verified on-site + against obs-h8819 + the test fixture |
| **96 kHz mode** | **40 channels**, 12 samples/packet (`REAC_MODE_96K = {96000, 40, 12}`) at 8000 pps — verified on-rig 2026-06-06: the 96 kHz stream measured ~8000 pps carrying the same 1492 B / 40-channel frame. 96 kHz keeps all 40 channels and doubles the packet rate; it does **not** halve channels |
| Channel cap | `REAC_MAX_CHANNELS 40` per REAC connection (at every rate) |
| Clock | REAC has **one clock master** (e.g. a digital mixer/console); the bridge only **observes** the clock, it does not generate it |
| Rate selection | **Set by config, not auto-detected** — the frame is a fixed 1492 B at every rate; the rate is carried by the **packet rate** (`pps = rate / 12`: 44.1 kHz = 3675 pps, 48 kHz = 4000 pps, 96 kHz = 8000 pps), not by the frame. Use `--rate 48000` or `--rate 96000`; any non-`96000` value falls back to 48 kHz mode |

> **96 kHz is rate-invariant, not channel-halving.** REAC carries 40
> channels at every sample rate in the same fixed 1492 B frame (40 ch × 12
> samples/packet × 3 B = 1440 B of audio). The sample rate is set by the
> packet rate, not the frame: `pps = rate / 12`, so 96 kHz runs at 8000 pps
> against 4000 pps at 48 kHz. The on-rig 96 kHz capture (2026-06-06)
> measured ~8000 pps carrying 40 channels, which confirms the double-pps
> model (`REAC_MODE_96K = {96000, 40, 12}`) and rules out the earlier
> channel-halving guess (`{96000, 20, 24}` would have measured ~4000 pps /
> 20 channels). 96 kHz audio output is verified, not pending.
>
> See the [REAC wire-format
> reference](https://github.com/FreeREAC/reac-protocol/blob/main/wire-format.md)
> for the full per-rate packetization.

## Related documentation

- [`README.md`](../README.md) — top-level repo overview, build/test/verify and demo quick start.
- [`docs/INSTALLATION.md`](INSTALLATION.md) — build the two apk packages, install on the router, build/run natively for testing.
- [`docs/CONFIGURATION.md`](CONFIGURATION.md) — UCI + CLI reference and the two-network separation model.
- [`docs/EXAMPLES.md`](EXAMPLES.md) — worked recipes (single zone, multi-zone, bench demo, SDP, offline analysis).
- [FreeREAC/reac-protocol](https://github.com/FreeREAC/reac-protocol) — the REAC wire-format reference (frame layout, per-rate packetization, EtherType, end marker).
- [FreeREAC/reac-lab](https://github.com/FreeREAC/reac-lab) — raw working material: captures, on-rig journals, and the design specs.
