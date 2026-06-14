<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
-->

# Publishing the REAC source in other formats — by routing

reac-pw puts the REAC fabric into the PipeWire graph as one node:
`reac:capture`, an `Audio/Source` with 40 mono float output ports
`capture_01`..`capture_40` (see [DESIGN.md](../DESIGN.md)). It carries
the decoded downstream broadcast — 40 channels at the recovered REAC
rate, already de-interleaved by the proven plain-LE core
(`reac-aes67/src/reac_decode.c`, `(s*40+ch)*3`) and fed through
libreac validate/counter/rate-detect into the lock-free ring.

The point of the node model: **you do not write a new encoder per
output format — you route**. Every target below is `reac:capture`'s
ports linked, with `pw-link` / WirePlumber / qpwgraph, to some other
PipeWire node. PipeWire's adapter does the channel-map, format
conversion and adaptive resample on the link; the far node does the
wire encoding. The REAC-specific code (capture, validate, decode,
counter-slope rate recovery) is all reused and lives upstream — this
document only describes how to wire the existing source into the
existing PipeWire output modules.

`reac-pw` itself never speaks any of these formats. AES67/RTP, AVB and
USB-gadget audio are all PipeWire (or ALSA/kernel) features that
already exist on a Fedora box; reac-pw's job ends at the 40-port
source.

Verified on the target: Fedora + PipeWire 1.4.11, which ships
`module-rtp-sink`, `module-rtp-source`, `module-rtp-sap`,
`module-rtp-session` and `module-avb` in `/usr/lib64/pipewire-0.3/`.

## What each output needs (honest summary)

| Output | Extra hardware | How |
|---|---|---|
| Local DAC / DAW / file | none (a soundcard for a DAC) | `pw-link` to an ALSA sink, Ardour, or a file sink |
| AES67 / RTP | none (a clean network helps) | `module-rtp-sink` + SAP on `239.255.255.255:9875` |
| AES67 + PTP (RFC 7273) | a PTP grandmaster or `ptp4l` on a PHC NIC | as AES67, plus a PTP-locked clock and the `ts-refclk` SDP line |
| Dante | **none beyond AES67+PTP** — there is no native Dante from Linux | only via Dante's AES67 mode, which is itself AES67+PTP |
| MADI (64 ch) | **yes** — an RME/ALSA MADI card (HDSPe MADI etc.) | `pw-link` to the MADI ALSA sink |
| USB Audio Class gadget | **yes** — a MiniPC/SBC with a USB *device/OTG* port | kernel `g_audio` gadget exposes an ALSA card; `pw-link` to it |
| AVB / Milan | **yes** — AVB-capable switch + endpoints + a TSN/AVB NIC | `module-avb` (or OpenAvnu) — infra-dependent |
| AES50 | **not softwareable at all** | proprietary non-Ethernet PHY — hardware bridge only |

REAC itself is 40 ch max, 24-bit, 44.1/48/96 kHz; anything wider (MADI's
64) is zero-padded or partially mapped, anything narrower takes a
channel subset. Pick the `capture_NN` ports you route accordingly.

---

## 1. Local DAC / DAW / file (no extra hardware)

The base case the node model was built for. Nothing beyond a soundcard.

Monitor on a DAC — link the 40 REAC ports onto a stereo (or
multichannel) ALSA sink. `pw-link` connects port-to-port; list first,
then wire the pair you want:

```
pw-link -o reac:capture          # list reac-pw's output ports
pw-link -i                       # list available input ports (sinks)
pw-link reac:capture:capture_01 alsa_output.<card>:playback_FL
pw-link reac:capture:capture_02 alsa_output.<card>:playback_FR
```

Record to a file — route into any recorder node, or capture with
`pw-record` (which creates its own input node you can link, or which
can target the source directly):

```
pw-record --target reac-capture reac_40ch.wav
```

Into a DAW — Ardour/Reaper under PipeWire expose input ports; link
`capture_01..NN` to the DAW's track inputs in qpwgraph or with
`pw-link`. The source advertises `node.rate = 1/<recovered rate>`, so
the DAW sees the desk's rate; if its own clock differs, PipeWire
async-resamples on the link (the Tier-A bridge — reac-pw publishes the
counter-slope ppm into `io_rate_match`, no work for you).

Clock note: by default `reac:capture` is a *follower* (the DAC/graph
drives, PipeWire resamples REAC into it). For a pure monitor you can run
REAC as the graph **driver** instead so everything is sample-locked to
the desk — see DESIGN.md "follower vs driver". That is a node flag, not
a different output.

---

## 2. AES67 / RTP (no extra hardware)

This is the canonical way to make REAC visible on an IP network. Route
`reac:capture` into PipeWire's `module-rtp-sink`; it RTP-packetises and
announces an SDP over SAP. No new code — it is the same hop
`reac-aes67` does as a monolithic bridge, except here AES67 is one
optional output of the graph rather than the hub.

Load an RTP sink that auto-links from the REAC source and announces on
the **AES67 standard discovery group `239.255.255.255:9875`** (PipeWire
defaults SAP to `224.0.0.56:9875`, which is fine Linux-to-Linux but is
*not* what stock AES67/Dante gear scans — set it explicitly):

```
pactl load-module module-rtp-sink \
  source.props='{ node.name = "reac-aes67-out" }' \
  stream.props='{
      sess.name        = "REAC 40ch"
      audio.format     = S24BE
      audio.rate       = 48000
      audio.channels   = 8
      destination.ip   = 239.69.0.1
      destination.port = 5004
      sess.sap.address = 239.255.255.255
      sess.sap.port    = 9875
      sess.ts-refclk   = ""
  }'
```

Then link the REAC ports onto the new RTP node's inputs (qpwgraph, or
`pw-link reac:capture:capture_01 reac-aes67-out:input_1`, ...). AES67's
common interop shape is L24 / 48 kHz / 8-channel @ 1 ms; for the full 40
channels either raise `audio.channels` or load several rtp-sink modules
fed from different `capture_NN` groups.

The reverse — receiving AES67 back into the graph — is
`module-rtp-source` / `module-rtp-sap`, which listen for SAP
announcements and auto-create receiver nodes.

SAP stale-origin trap (already hit once on the rig): receivers pin the
first-seen SDP per stream *name* and ignore later changes. If you change
format/rate/channels, bump the SDP origin (`o=`) or change `sess.name`
so receivers re-read it.

Plain Linux/PipeWire receivers lock this immediately. Cross-vendor AES67
hardware needs PTP — next section.

---

## 3. AES67 + PTP, RFC 7273 (needs a PTP clock)

Pro AES67/Dante receivers will not lock a flow without a shared PTP
media clock and the RFC 7273 reference-clock line in the SDP. This needs
**extra setup but not exotic hardware**: a PTP grandmaster on the
segment, or `ptp4l` running on a PHC-capable NIC (an Intel i210/i225/i226
has an on-die `/dev/ptpN`). REAC carries no PTP and no clock of its own —
PTP is purely the *output* network's clock, layered on at the AES67 hop.

Run PTP on the AES67 NIC (never on the REAC NIC — REAC has no PTP and
forcing it there only adds jitter to the clock-slave path):

```
ptp4l -i <aes67-nic> -f /etc/ptp4l-aes67.conf -m
phc2sys -a -r                     # discipline system/PHC as needed
```

Then add the RFC 7273 reference-clock to the rtp-sink so the SDP carries
`a=ts-refclk:ptp=IEEE1588-2008:<gmid>` + `a=mediaclk:direct=0`:

```
pactl load-module module-rtp-sink stream.props='{
    sess.name        = "REAC 40ch PTP"
    audio.format     = S24BE
    audio.rate       = 48000
    audio.channels   = 8
    destination.ip   = 239.69.0.1
    sess.sap.address = 239.255.255.255
    sess.ts-refclk   = "ptp=IEEE1588-2008:<grandmaster-id>:0"
    sess.media-clock = "direct=0"
}'
```

The REAC source is still a graph follower; PipeWire async-resamples the
REAC clock onto the PTP-disciplined output clock on the link. So the
desk's audio is bit-rate-reconciled into the PTP timebase by the
resampler — there is no hardware lock between the REAC wire and PTP, and
there does not need to be for AES67/Dante to accept the flow.

---

## 4. Dante (only via AES67 + PTP — no native Dante from Linux)

**There is no native Dante transmitter on Linux.** Dante's wire protocol
is proprietary (Audinate); the only standards-based way in is Dante's
**AES67 mode**, which a Dante device exposes as ordinary AES67 multicast
flows. So "REAC → Dante" is exactly section 3 (AES67 + PTP) with the
receiver being a Dante device in AES67 mode — there is nothing
Dante-specific to configure on the Linux side beyond getting AES67+PTP
right.

What Dante demands that casual AES67 does not:

- **PTP-locked clock + RFC 7273 in the SDP** — Dante will not subscribe
  an AES67 flow without it (section 3). This is the hard gate.
- **The AES67 discovery group** `239.255.255.255:9875`, not PipeWire's
  default — so the Dante Controller / device actually sees the SAP
  announcement.
- AES67-compatible flow shape: L24, 48 kHz, 1 ms packet time, channel
  counts the receiver supports (commonly 8).
- On the Dante device, AES67 mode enabled and the flow subscribed in
  Dante Controller.

`reac-aes67` already carries a `--profile dante` that sets exactly these
(AES67 group, L24/48k, RFC 7273 clock line). In the routed model you
reproduce it by pointing `module-rtp-sink` at the AES67 group with the
`ts-refclk`/`media-clock` props above and running `ptp4l`. No extra
hardware beyond the PTP source — but the PTP discipline is non-optional,
and a misordering/offload-heavy NIC will fail Dante's tighter timing.

---

## 5. MADI — 64 channels (needs an RME/ALSA MADI card)

**Needs hardware:** a MADI interface with an ALSA driver — RME HDSPe
MADI / MADI FX (`snd-hdspm`), or any card that presents a 64-channel
ALSA playback device. There is no software MADI; MADI is a coax/optical
serial format a card serialises in hardware.

With the card present it is just another ALSA sink in the graph, so the
route is identical to the DAC case — only wider:

```
pw-link -i | grep -i madi          # find the MADI playback ports
pw-link reac:capture:capture_01 alsa_output.<madi-card>:playback_1
pw-link reac:capture:capture_02 alsa_output.<madi-card>:playback_2
# ... up to capture_40 -> playback_40
```

REAC is 40 ch; MADI carries 64. Channels 1..40 map straight across;
41..64 stay silent (or carry whatever else you patch in the graph).
PipeWire resamples REAC↔card if the MADI clock differs, or — better for
MADI — run the card as the graph driver and word-clock the rig and the
MADI card off one house clock (Tier B in DESIGN.md) for a sample-locked,
resampler-free path.

---

## 6. USB Audio Class gadget — the MiniPC as a USB soundcard

**Needs the right hardware port:** a board whose USB controller can run
in **device/OTG** mode (a MiniPC OTG port, or an SBC like a Pi/rk3588
board with a peripheral-capable port). A normal PC's host-only USB-A
ports cannot be a gadget. This reuses the existing USB path on the
endpoint — the box appears to an upstream host as a standard USB Audio
Class 2.0 soundcard.

Bring up the kernel UAC2 gadget (`g_audio` / configfs), which creates an
ALSA card on the gadget side:

```
modprobe g_audio c_chmask=0 p_chmask=0xff p_srate=48000 p_ssize=3
# or the configfs/libcomposite equivalent for a composite gadget;
# p_chmask sets the playback channel count the host sees, p_ssize=3 = 24-bit
aplay -l    # the gadget shows up as a new playback card
```

Then route REAC onto that ALSA sink exactly like a local DAC:

```
pw-link reac:capture:capture_01 alsa_output.<gadget-card>:playback_1
# ...
```

The host on the other end of the USB cable then sees REAC audio as a
plain USB microphone/soundcard input. UAC2 channel counts above 8 are
brittle across host OSes, so for >8 channels expect to tune `p_chmask`
and test against the specific host. Clocking: the USB host is the master
of the UAC clock domain, so REAC is the graph follower and PipeWire
resamples REAC into the USB rate — the standard Tier-A path.

---

## 7. AVB / Milan (needs AVB infrastructure)

**Needs hardware + a whole network:** AVB/TSN-capable switches, an
AVB-capable NIC (Intel i210 with AVB queues, or a TSN NIC), and
endpoints that speak AVB/Milan. AVB is not a thing you turn on over a
normal LAN — it needs the switch fabric reserving bandwidth (SRP/MSRP),
gPTP (802.1AS) time sync, and credit-based shaping.

Two software paths exist on Fedora:

- **PipeWire `module-avb`** — present in 1.4.11
  (`libpipewire-module-avb.so`). It can publish/consume AVB streams when
  the NIC and switch support AVB; route `reac:capture` into the AVB sink
  node it creates, same `pw-link` pattern as the others.
- **OpenAvnu** (`gptp`, `maap`, the AVTP pipeline) — the reference stack
  if you need full Milan endpoint behaviour or the PipeWire module's
  coverage falls short.

Either way the gating factor is the **AVB infrastructure**, not reac-pw:
without an AVB switch and gPTP grandmaster there is nothing to stream
to. With the infra present it is one more routed output of the same
source node. gPTP runs on the AVB NIC, never on the REAC NIC.

---

## 8. AES50 — not softwareable

**Cannot be done in software at all.** AES50 (SuperMAC / HyperMAC, Klark
Teknik / Midas) uses a **proprietary non-Ethernet PHY** — it runs custom
signalling over the copper, not standard 100BASE-TX/1000BASE-T framing a
NIC and a raw socket can produce. There is no kernel driver, no
`module-*`, and no AF_PACKET path that emits AES50, because a commodity
Ethernet PHY physically cannot generate the line code AES50 expects.

The only REAC↔AES50 bridge is **hardware**: an AES50 interface
ASIC/board between the two. Nothing in reac-pw or PipeWire can produce
AES50; it is listed here only to record that it is out of scope by
physics, not by missing software.

---

## Routing recap

```
                                  ┌─► ALSA/DAC sink        (§1, no extra hw)
                                  ├─► pw-record / file     (§1)
                                  ├─► Ardour/DAW inputs    (§1)
  reac:capture  (40 F32 ports)    ├─► module-rtp-sink ─► AES67/RTP      (§2)
  capture_01..capture_40   ──────►├─► module-rtp-sink + ptp4l ─► AES67+PTP / Dante (§3, §4)
  (the only thing reac-pw owns)   ├─► ALSA MADI card sink  (§5, needs RME)
                                  ├─► g_audio UAC2 gadget  (§6, needs OTG port)
                                  └─► module-avb / OpenAvnu (§7, needs AVB infra)

  AES50: hardware bridge only — no software path exists.       (§8)
```

One source node, one `pw-link` per destination. The encoders are
PipeWire modules and kernel drivers that already exist; reac-pw
contributes only the validated, decoded REAC source they all draw from.
