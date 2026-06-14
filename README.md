# reac-aes67

Router-side bridge that decodes Roland **REAC** (audio-over-Ethernet,
EtherType `0x8819`) on an OpenWrt router and re-emits it as **AES67** (RTP L24
multicast) so any Linux box receives all REAC channels in PipeWire — record,
monitor, OBS, route — with no dedicated capture NIC and no bespoke receiver
software.

Personal audio project (lives under the `FreeREAC` org alongside
[`reac-tools`](https://github.com/FreeREAC/reac-tools) and
[`reac-docs`](https://github.com/FreeREAC/reac-docs)).

**Status:** M2 complete; OpenWrt apk packages cross-built for **both** supported
targets — aarch64 (`mediatek/filogic`, e.g. GL-MT6000 / Flint 2) and mips
(`ramips/mt7621`, e.g. Cudy WR2100). The daemon's decode/clock/PLC/RTP core is
portable, dependency-free C, so it builds for any OpenWrt target.
Full path — AF_PACKET capture → decode → media-clock → **PLC** → RTP L24 →
UDP/AES67 multicast — with **39 tests + cross-tool verification, all green** and
a live loopback smoke test. Two installable `.apk` per target for OpenWrt
25.12.4: `reac-aes67` (daemon + UCI + procd + ubus stats, links real libubus)
and `luci-app-reac-aes67` (config form, live status, SDP). **Validated
end-to-end on hardware on a Cudy WR2100** (`ramips/mt7621`: SAP discovery → a
40-channel PipeWire node, with LuCI + ubus live); on-device smoke on the
GL-MT6000 (`mediatek/filogic`) is still pending.

**Documentation** ([`docs/`](docs/README.md)):
[Overview & use cases](docs/OVERVIEW.md) ·
[Installation](docs/INSTALLATION.md) ·
[Configuration](docs/CONFIGURATION.md) ·
[Examples](docs/EXAMPLES.md).

**Recording a live wired REAC network?** Tap the desk's REAC output and record all 40
channels without disturbing the link — see [Examples → recipe (f)](docs/EXAMPLES.md).
The bridge only ever *listens* on REAC (no TX, no handshake), so it can't interfere.

```sh
make test     # 39 tests against the real reac-tools fixture
make verify   # C decoder vs independent Python reference — sample-for-sample
make          # build the reac-aes67 CLI (pcap replay / live capture)
```

### Verification ("our own tools + captures")

`make verify` decodes the real `reac-tools` capture with the C decoder and
with an independent Python reference (replicating obs-h8819's de-interleave),
then asserts they match sample-for-sample. Result: **4 frames, 1920 samples,
exact match** — a three-way agreement (obs-h8819 ⇄ our C ⇄ Python) on real
bytes. The pipeline test additionally proves decode→clock→RTP end-to-end on
that capture, including **packet loss concealment**: repeat-last + linear
crossfade, with a fallback ladder (crossfade → burst gain-fade → hold-last →
silence-on-cold-start-only). Hold-last is the floor — never silence on live
audio (operator preference). Search-free + integer, so a whole-frame loss
across all streams stays inside the 250 µs packet slot.

### Verified facts (primary sources)

- **Rate-invariant framing**: every rate uses the same 50 B header + 1440 B
  audio (12 samples × 40 ch × 3 B) + `0xC2 0xEA`, 1492 B. The rate is carried by
  the **packet rate**, not the frame: pps = rate / 12 (44.1 kHz = 3675 pps,
  48 kHz = 4000 pps, 96 kHz = 8000 pps). Decode verified against the reac-tools
  pcap fixture. Full reference:
  [wire-format.md](https://github.com/FreeREAC/reac-protocol/blob/main/wire-format.md).
- **96 kHz keeps 40 channels**: same 1440 B frame (12 samples × 40 ch × 3 B) at
  double the packet rate (~8000 pps). On-rig the 96 kHz stream measured ~8000 pps
  carrying 40 channels — verified, not pending. The earlier channel-halving model
  (20 ch / 24 samples at 96 kHz) is rejected.
- **Clock**: REAC has one master (V-Mixer by default; "mis-matched sampling
  rates → failure of audio transmission") — per the R-1000 manual. The bridge
  only observes it.
- Rate (44.1/48/96 k) is detectable from the wire only via the **packet rate**
  (the frame is identical) → set by config.

## In one picture

```
REAC zones A/B/C (≤3, all clock-locked to the one REAC master / mixer)
   → router daemon: AF_PACKET 0x8819 → decode (24-bit PCM, per reac_mode)
   → sample-counting media clock (PLC conceals lost packets, not silence)
   → AES67 / RTP L24 multicast (one stream per REAC zone)
   → stock PipeWire on any Linux box → patchable ports per zone
       (40 ch at any rate; 48 kHz = 4000 pps, 96 kHz = 8000 pps)
```

## Build the OpenWrt packages

The daemon is portable C with no special host deps; the OpenWrt **x86_64
cross-SDK** builds the `.apk` (no qemu/emulation needed — it cross-compiles).
The same helper script builds for either supported board family — just point it
at the matching SDK:

| Target family | Example device | Package arch |
| --- | --- | --- |
| `mediatek/filogic` (aarch64) | GL-MT6000 / Flint 2 | `aarch64_cortex-a53` |
| `ramips/mt7621` (mips) | Cudy WR2100 | `mipsel_24kc` |

The helper does it in a throwaway Fedora container:

```sh
cd .build
# pick the SDK matching your board — filogic (aarch64) shown; for MIPS use
#   .../targets/ramips/mt7621/openwrt-sdk-25.12.4-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64.tar.zst
curl -fsSLO https://downloads.openwrt.org/releases/25.12.4/targets/mediatek/filogic/openwrt-sdk-25.12.4-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64.tar.zst
mv openwrt-sdk-*.tar.zst sdk.tar.zst
podman run --rm -v "$PWD":/work:Z -v "$PWD/..":/repo:ro,Z \
    registry.fedoraproject.org/fedora:43 bash /work/build-apk.sh
ls out/   # reac-aes67-*.apk  luci-app-reac-aes67-*.apk
```

`build-apk.sh` is target-agnostic — it derives the arch from whichever SDK you
unpack (override `SDK_TAR`/`SDK_DIR`/`OUT_DIR` to build several side by side). It
installs SDK build deps via `dnf`, unpacks the SDK, sets up feeds (incl. `luci`),
drops in `openwrt/reac-aes67` + `openwrt/luci-app-reac-aes67` and the `src/`
tree, then `make package/<name>/compile`. Output is two apk for the SDK's arch
(`aarch64_cortex-a53` or `mipsel_24kc`) in `.build/out/`. (`.build/sdk`,
`.build/out`, and the SDK tarball are gitignored.)

> Notes: the package recipe stages `src/` into `PKG_BUILD_DIR` via `Build/Prepare`
> and compiles with `-DHAVE_UBUS` so the real libubus stats binding is linked
> (the daemon ELF `NEEDs libubus.so/libblobmsg_json/libubox`). apk-tools v3
> packages aren't tar/zstd — inspect via the SDK's `apk` or the build's
> `.pkgdir` tree, not `tar tzf`.

## Local demo (no hardware): a 40-channel AES67 stream you can see

The daemon can synthesize a continuous, real-time-paced 40-channel tone and
emit it as AES67 — so you can watch a 40-channel node appear in PipeWire
without the REAC rig:

```sh
make
./demo/demo.sh          # sender + PipeWire receiver, asserts the 40ch node appears
KEEP=1 ./demo/demo.sh   # leave it running: wpctl status | grep reac, qpwgraph, pw-record …
```

Under the hood:

```sh
# sender: 40 ch @ 48 kHz, +25 Hz per channel, AES67 multicast, ~4000 pps
./build/reac-aes67 --gen-tone --rate 48000 --udp 239.69.0.1:5004 --tone-detune 25
# receiver (persistent): drop demo/reac-a-rtp-source.conf into
#   ~/.config/pipewire/pipewire.conf.d/ and restart pipewire
```

Verified locally: the stream is 3992 RTP L24 pkts/s (1452 B each, monotonic seq)
and PipeWire shows a 40-channel `Audio/Source` node. (`pw-cli load-module`
unloads when pw-cli exits — for a lasting receiver use the `.conf` drop-in.)

## Two networks: keep AES67 off the REAC segment

REAC is the timing-critical clock domain and is heavy (~48 Mbit/s broadcast per
zone at 48 k, more at 96 k); the rig isolates it on its own VLANs/ports
(lan1/2/3 = VLAN 11/12/13). **AES67 egress must live on a separate L2 network**
so it never shares airtime/flooding with REAC. No daemon change is needed — the
kernel routes each stream's RTP out whichever interface owns the route to its
multicast group. So:

- Put the AES67 multicast subnet (e.g. `239.69.0.0/24`) on a dedicated **`aes67`
  bridge**, which can carry a separate **Wi-Fi SSID** and/or a **reserved
  ethernet port** — see [`openwrt/files/network-aes67.example`](openwrt/files/network-aes67.example).
- Set each stream's `mcast_addr` into that subnet; AES67 then leaves the router
  only on the `aes67` segment, REAC stays isolated on its capture interfaces.

```
 REAC zones (lan1/2/3, VLAN 11/12/13)  ── captured raw (AF_PACKET) ──┐
                                                                      ▼
                                                            reac-aes67 daemon
                                                                      │ RTP L24
 AES67 net ('aes67' bridge: SSID and/or a port) ◄── multicast egress ─┘
   └─ PipeWire receivers subscribe here (never on the REAC segment)
```

## Install on the router (OpenWrt 25.12 — aarch64 or mips)

```sh
scp .build/out/*.apk root@<router>:/tmp/
ssh root@<router> 'apk add --allow-untrusted /tmp/reac-aes67-*.apk /tmp/luci-app-reac-aes67-*.apk'
# edit /etc/config/reac-aes67 (iface, rate, multicast, ssrc), then:
ssh root@<router> 'service reac-aes67 enable && service reac-aes67 start'
# live stats: ubus call reac-aes67.REAC-A status   ·   LuCI: Services → REAC → AES67
```

The daemon runs one instance per enabled `stream` section (procd), each
capturing one REAC zone and emitting its AES67 multicast. Per-stream live stats
are published over ubus as `reac-aes67.<name>` and shown on the LuCI status page.

## References

- REAC protocol: `github.com/per-gron/reacdriver`,
  `github.com/norihiro/obs-h8819-source` (decode lifted from
  `convert_to_pcm24lep`).
- `github.com/norihiro/reaccapture` (GPL-3.0) — Linux REAC pcap→WAV decoder, same
  lineage; reference for the **MASTER_ANNOUNCE/handshake decode** (type `0xCFEA`,
  `data[6]==0x0D`) and **both s24be/s24le sample justifications** (the parts
  obs-h8819 omits). Used as a protocol reference (no code copied).
- Analysis + the shared test fixture: `github.com/FreeREAC/reac-tools`.

## Acknowledgements

This bridge stands on the REAC reverse-engineering and decode work done by
others, and on public IETF/AES standards. The REAC audio de-interleave is
adapted from Norihiro Kamae's obs-h8819-source (GPL-3.0-or-later); the
underlying REAC wire framing (EtherType 0x8819, 1492-byte frames, the L2 counter
and end marker) was originally documented by per-gron's reacdriver. The AES67
output side is an original implementation of the relevant standards. See
[NOTICE](NOTICE) for the full per-file attribution.

- [norihiro/obs-h8819-source](https://github.com/norihiro/obs-h8819-source) —
  REAC capture/decode for OBS; source of the de-interleave math
  (GPL-3.0-or-later, Copyright (C) 2022 Norihiro Kamae).
- [per-gron/reacdriver](https://github.com/per-gron/reacdriver) — original REAC
  reverse-engineering and wire-framing reference (GPL-3.0).
- [norihiro/reaccapture](https://github.com/norihiro/reaccapture) (GPL-3.0) —
  Linux REAC decoder carrying the MASTER_ANNOUNCE/handshake decode and both
  s24be/s24le justifications; reference for the bidirectional-master path and the
  24-bit justification question (no code copied).
- Roland R-1000 Owner's Manual — clock-master behaviour and sample-rate notes.
- IETF RFC 3550 (RTP), RFC 2974 (SAP), RFC 4566 (SDP), and AES67 — the standards
  implemented on the AES67 output side.
- The classic libpcap savefile format — for offline replay/testing.

## License

GPL-3.0-or-later. Copyright (C) 2026 Pau Aliagas. See [LICENSE](LICENSE) and
[NOTICE](NOTICE).
