# reac-aes67 Documentation

`reac-aes67` is a C daemon that decodes Roland **REAC** (audio-over-Ethernet,
EtherType `0x8819`) on an OpenWrt 25.12 router (aarch64 `mediatek/filogic` or
mips `ramips/mt7621`) and re-emits it
as **AES67** (RTP L24 multicast) for a standard Linux audio graph (PipeWire).
The decode/clock/PLC/RTP core is dependency-free (`-lm`); on the router it ships
as two apk packages — the daemon (`reac-aes67`) and a LuCI web app
(`luci-app-reac-aes67`).

## Guides

- [Overview & Use Cases](OVERVIEW.md) — what it is, the problem it solves, how
  the audio flows (decode → media clock → PLC → RTP L24 → AES67 send), use
  cases, and the protocol facts. REAC is rate-invariant: 40 channels ×
  12 samples/packet in a fixed 1492 B frame at every rate, with the sample rate
  set by the packet rate (pps = rate / 12 → 44.1 kHz = 3675 pps, 48 kHz =
  4000 pps, 96 kHz = 8000 pps; 96 kHz keeps 40 channels and doubles the pps).
- [Installation](INSTALLATION.md) — build the two `.apk` from source with the
  OpenWrt cross-SDK in a container, install on the router (procd + `CAP_NET_RAW`
  via the capabilities file), and optionally build/run/test the daemon natively
  on a Linux host (`make`, `make test`, `make verify`).
- [Configuration Reference](CONFIGURATION.md) — UCI schema
  (`/etc/config/reac-aes67`), the procd init's UCI→flag mapping, the full CLI
  flag reference and mode precedence, the two-network AES67 separation model,
  and SDP/ubus discovery.
- [Worked Examples & Recipes](EXAMPLES.md) — single zone, multi-zone on a
  dedicated AES67 network, the no-hardware `--gen-tone` bench demo, printing an
  SDP, and offline pcap analysis (replay / count / dump / cross-verify).

## Design specs

- [M5000 AES/EBU conversion](design/specs/m5000-aes-ebu-conversion.md)
  — converting REAC to and from AES/EBU for the Roland M5000.

## Moved material

The raw working material and the full protocol reference now live in sibling
[FreeREAC](https://github.com/FreeREAC) repos:

- [reac-protocol](https://github.com/FreeREAC/reac-protocol) — the REAC wire
  format reference, including the rate-from-pps model
  ([wire-format.md](https://github.com/FreeREAC/reac-protocol/blob/main/wire-format.md)).
- [reac-lab](https://github.com/FreeREAC/reac-lab) — on-rig runbooks, capture
  campaigns, the dated design specs, and the re-pacer / Dante-interop work.
- [reac-docs](https://github.com/FreeREAC/reac-docs) — documentation hub.

## Repo root

- [README.md](../README.md) — project overview, status, build/test/verify and
  demo quick start.
