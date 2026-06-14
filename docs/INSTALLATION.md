# Installation

This guide covers building the two OpenWrt `.apk` packages from source,
installing them on an OpenWrt 25.12 router (aarch64 `mediatek/filogic` or mips
`ramips/mt7621`), and (optionally)
building and running the daemon natively on a Linux host for testing.

`reac-aes67` ships as **two** OpenWrt packages, both built from this one repo:

| Package | Contents | Key recipe fields |
| --- | --- | --- |
| `reac-aes67` | the C daemon (`/usr/bin/reac-aes67`), UCI config, procd init, capabilities file | `SECTION:=sound` / `CATEGORY:=Sound`, `DEPENDS:=+libubus +libblobmsg-json` |
| `luci-app-reac-aes67` | client-side LuCI app (config form + live status views) | `LUCI_DEPENDS:=+reac-aes67`, `LUCI_PKGARCH:=all` |

Both are `PKG_VERSION:=0.1.0`, `PKG_RELEASE:=1`, `GPL-3.0-or-later`.

---

## 1. Build the two `.apk` from source (cross-SDK in a container)

The daemon is portable C; the OpenWrt **x86_64 cross-SDK** produces the `.apk`
for the router. This is a true cross-compile — **no qemu / emulation / target
build host is needed**. The build runs inside a container that has the OpenWrt
SDK build dependencies (the helper script targets
`registry.fedoraproject.org/fedora:43`).

Two board families are supported; pick the SDK that matches your router (under
`downloads.openwrt.org/releases/25.12.4/targets/…`):

| Target family | Example device | SDK tarball | Package arch |
| --- | --- | --- | --- |
| `mediatek/filogic` (aarch64) | GL-MT6000 / Flint 2 | `mediatek/filogic/openwrt-sdk-25.12.4-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64.tar.zst` | `aarch64_cortex-a53` |
| `ramips/mt7621` (mips) | Cudy WR2100 | `ramips/mt7621/openwrt-sdk-25.12.4-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64.tar.zst` | `mipsel_24kc` |

`build-apk.sh` is target-agnostic: it derives the arch from whichever SDK you
unpack. Build both by re-running it with different `SDK_TAR`/`SDK_DIR`/`OUT_DIR`.

### Prerequisites

- A container runtime (`podman` is used in the examples below; `docker` works
  with the same volume/`-v` flags).
- Network access to fetch the OpenWrt SDK tarball and the package feeds.
- A checkout of this repo. The build helper expects to run with two mounts:
  `/work` = the repo's `.build/` directory, `/repo` = the repo root (read-only).

### Steps

The helper script `.build/build-apk.sh` automates the whole pipeline. From the
repo's `.build/` directory:

1. Fetch the OpenWrt SDK for your target (see the table above) and place it at
   `.build/sdk.tar.zst`. The build helper reads it from `SDK_TAR=/work/sdk.tar.zst`.

   ```sh
   # aarch64 (mediatek/filogic) shown; for MIPS use the ramips/mt7621 SDK instead
   cd .build && curl -fsSLO <SDK_URL>/openwrt-sdk-25.12.4-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64.tar.zst && mv openwrt-sdk-*.tar.zst sdk.tar.zst
   ```

2. Run the build inside the container, mounting `.build/` as `/work` and the
   repo root as `/repo` (read-only):

   ```sh
   podman run --rm -v "$PWD":/work:Z -v "$PWD/..":/repo:ro,Z registry.fedoraproject.org/fedora:43 bash /work/build-apk.sh
   ```

3. Collect the two artifacts:

   ```sh
   ls .build/out/   # reac-aes67-*.apk  luci-app-reac-aes67-*.apk
   ```

### What `build-apk.sh` does

The script (`SDK_TAR=/work/sdk.tar.zst`, `SDK_DIR=/work/sdk`, `REPO=/repo`) runs
these phases:

1. **Install build deps (RPM):** `gcc gcc-c++ make git python3 ncurses-devel
   zlib-devel zstd wget which unzip file rsync perl gawk diffutils findutils tar
   xz bzip2 patch`.
2. **Unpack the SDK** (only if `$SDK_DIR` is absent):
   `tar --use-compress-program=unzstd -xf "$SDK_TAR" -C "$SDK_DIR" --strip-components=1`.
3. **Set up feeds** (first run only — idempotent once `feeds/luci` exists):
   `cp feeds.conf.default feeds.conf`, `./scripts/feeds update -a`,
   `./scripts/feeds install luci-base`.
4. **Drop in the two packages:** copies `openwrt/reac-aes67/Makefile` plus the
   whole `src/*.c`/`src/*.h` tree and the three `openwrt/files/` files
   (`reac-aes67.config`, `reac-aes67.init`, `capabilities-reac-aes67.json`) into
   `package/reac-aes67/`, and copies the whole `openwrt/luci-app-reac-aes67/`
   directory into `package/luci-app-reac-aes67/`.
5. **Configure:** `make defconfig`, append `CONFIG_PACKAGE_reac-aes67=y` and
   `CONFIG_PACKAGE_luci-app-reac-aes67=y` to `.config`, `make defconfig` again.
6. **Compile each package:** `make package/reac-aes67/compile V=s -j"$(nproc)"`
   then `make package/luci-app-reac-aes67/compile V=s -j"$(nproc)"`.
7. **Collect artifacts:** finds the `*.apk` and copies them to `/work/out`
   (i.e. `.build/out/`).

### How the daemon is compiled

The daemon recipe (`openwrt/reac-aes67/Makefile`) has **no `PKG_SOURCE`** — it
vendors the C sources. `Build/Prepare` stages them:

```make
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)/src
	$(CP) ./src/*.c ./src/*.h $(PKG_BUILD_DIR)/src/
endef
```

`Build/Compile` links the daemon in one `$(TARGET_CC)` invocation with
`-std=c11 -O2 -DHAVE_UBUS -I$(PKG_BUILD_DIR)/src` and the libs
`-lubus -lblobmsg_json -lubox`. The sources linked into the binary are: `main.c`,
`reac_decode.c`, `media_clock.c`, `pcap_source.c`, `rtp_l24.c`, `plc.c`,
`pipeline.c`, `aes67_send.c`, `reac_capture.c`, `sdp.c`, `ubus_stats.c`.

`-DHAVE_UBUS` is mandatory in the OpenWrt build: it links the real libubus stats
binding (`ubus_stats.c`) instead of the no-op stub. Without it, the per-stream
`reac-aes67.<name>` ubus object would not exist and the LuCI status view would
show nothing.

The daemon recipe relies on `src/` being present alongside the recipe at build
time. `build-apk.sh` copies it in; a bare checkout of only `openwrt/` would have
an empty `src/` and `Build/Prepare`'s `$(CP)` would fail.

> **Inspecting the result:** OpenWrt 25.12 emits apk-tools v3 `.apk` packages.
> These are **not** tar/zstd archives — `tar tzf` will not read them. Inspect
> with the SDK's own `apk` tool or the build's `.pkgdir` tree.

---

## 2. Install on an OpenWrt 25.12 router (aarch64 or mips)

### Prerequisites

- An OpenWrt 25.12 router reachable over SSH — `mediatek/filogic` (aarch64) or
  `ramips/mt7621` (mips).
- The two `.apk` from step 1, matching the router's arch (`aarch64_cortex-a53`
  for filogic, `mipsel_24kc` for ramips/mt7621).
- The runtime libraries pulled by the daemon's `DEPENDS:=+libubus
  +libblobmsg-json`. The daemon ELF `NEED`s `libubus.so`, `libblobmsg_json`, and
  `libubox` at runtime; `apk add` resolves `libubus` and `libblobmsg-json`
  (which in turn pulls `libubox`) from the package repo.

### Steps

1. Copy both packages to the router and install them together (so the
   `luci-app-reac-aes67 → reac-aes67` dependency resolves in one transaction):

   ```sh
   scp .build/out/*.apk root@<ROUTER>:/tmp/ && ssh root@<ROUTER> 'apk add --allow-untrusted /tmp/reac-aes67-*.apk /tmp/luci-app-reac-aes67-*.apk'
   ```

   `--allow-untrusted` is needed because these are locally built, unsigned `.apk`.

2. The daemon package installs four paths:
   - `/usr/bin/reac-aes67` — the daemon binary.
   - `/etc/config/reac-aes67` — UCI config (declared as a `conffile`, so it
     survives upgrades).
   - `/etc/init.d/reac-aes67` — the procd init script.
   - `/etc/capabilities/reac-aes67.json` — the procd capabilities file.

3. Edit `/etc/config/reac-aes67`. The file has one `config reac-aes67 'global'`
   section with a master `option enabled '1'`, plus one or more
   `config stream '<id>'` sections (the template ships stream `a` enabled and
   `b`/`c` commented out). Per-stream options:

   | Option | Meaning | Template default (stream `a`) |
   | --- | --- | --- |
   | `enabled` | start this stream | `1` |
   | `name` | stream/session name (SDP + ubus object) | `REAC-A` |
   | `iface` | REAC capture interface | `<REAC_IFACE>` (e.g. `lan1`) |
   | `rate` | `48000` or `96000` | `48000` |
   | `mcast_addr` | AES67 multicast group | `<MCAST_GROUP>` (e.g. `239.69.0.1`) |
   | `mcast_port` | AES67 RTP port | `<PORT>` (e.g. `5004`) |
   | `payload_type` | RTP payload type (96–127) | `97` |
   | `ssrc` | RTP SSRC (hex) | `11223344` |
   | `ttl` | multicast TTL (1–255) | `1` |
   | `plc_xfade` | PLC crossfade width (0 = default) | (commented) |
   | `plc_fade_pkts` | PLC burst-fade length (0 = default) | (commented) |

   > **You MUST set `rate` to match the REAC clock master** (e.g. a digital
   > mixer/console). The rate is **not** auto-detectable from the wire — every
   > rate uses the same 1492-byte frame carrying 40 channels; the rate is set by
   > the packet rate (`pps = rate / 12`): `48000` → 4000 pps, `96000` → 8000 pps.
   > Both carry 40 channels — 96 kHz doubles the packet rate, it does not halve
   > the channel count. A wrong rate produces a garbled de-interleave, not an
   > error.

   > **Keep AES67 off the REAC segment.** No daemon change is needed — the kernel
   > routes each stream's RTP out whichever interface owns the route to its
   > multicast group. Put `mcast_addr` inside a subnet carried by a dedicated
   > `aes67` bridge (separate VLAN/SSID/port). See
   > [`openwrt/files/network-aes67.example`](../openwrt/files/network-aes67.example).
   > Never bridge the `aes67` interface with the REAC capture interfaces.

4. Enable and start the service:

   ```sh
   ssh root@<ROUTER> 'service reac-aes67 enable && service reac-aes67 start'
   ```

### How the service runs (procd + CAP_NET_RAW)

The procd init (`/etc/init.d/reac-aes67`, `USE_PROCD=1`, `START=95`, `STOP=10`,
`PROG=/usr/bin/reac-aes67`) spawns **one daemon instance per enabled `stream`
section** via `config_foreach start_stream stream`. If `global.enabled` is not
`1`, `start_service` logs `globally disabled` and starts nothing.

Each enabled stream is validated with `uci_validate_section` (typed validators
including `rate:or("48000","96000"):48000` and `payload_type:range(96,127):97`),
then launched with the UCI options mapped to daemon flags:

```
/usr/bin/reac-aes67 --listen $iface --udp ${mcast_addr}:${mcast_port} \
    --rate $rate --pt $payload_type --ssrc $ssrc --ttl $ttl --name ${name:-$cfg}
```

`plc_xfade > 0` appends `--plc-xfade $plc_xfade`; `plc_fade_pkts > 0` appends
`--plc-fade $plc_fade_pkts`.

Raw `AF_PACKET` capture requires `CAP_NET_RAW`. The daemon does **not** run as
full root; procd drops privileges to just that capability via
`procd_set_param capabilities /etc/capabilities/reac-aes67.json`. That file
grants only `net_raw` (in all four sets: `bounding`, `effective`, `inheritable`,
`permitted`) under the procd instance key `@reac-aes67`. Renaming that key
silently disables the grant, and capture then fails at runtime.

Each instance is `procd_open_instance "reac-$cfg"`, respawns with
`procd_set_param respawn 3600 5 0`, and reacts to config changes via
`procd_add_reload_trigger reac-aes67`.

### Verify

Check the service is up and an instance is running per enabled stream:

```sh
ssh root@<ROUTER> 'service reac-aes67 status; logread -e reac-aes67 | tail'
```

Confirm the per-stream ubus object is registered and query its live status. The
object is named `reac-aes67.<name>` (the `name` from the stream's UCI section)
and exposes a single `status` method:

```sh
ssh root@<ROUTER> 'ubus list | grep reac-aes67; ubus call reac-aes67.<NAME> status'
```

The `status` reply includes: `name`, `iface`, `rate`, `channels`, `mcast`,
`ssrc`, `running`, `uptime_s`, `packets_per_sec`, `rtp_seq`, `loss_total`,
`packets`, `concealed`, and `plc_tier` (a label: `clean` / `crossfade` /
`burst_fade` / `hold_last` / `silence`).

> The ubus object only exists when the daemon was built with `-DHAVE_UBUS`
> (which the OpenWrt build does). If `ubus list` shows no `reac-aes67.*` entry,
> the service is not running, or `global.enabled`/the stream is disabled.

In the LuCI web UI the app appears under **Services → REAC → AES67**; the
default landing page is the live **Status** table (polled every 2 s), with the
config form under **Streams**.

---

## 3. Optional: build and run the daemon natively on a Linux host (testing)

The decode/clock/pcap/rtp/pipeline/PLC/sdp core is dependency-free (only `-lm`),
so the daemon builds and the test suite runs on any Linux host. ubus is a
compile-time feature: without `-DHAVE_UBUS` the daemon links the no-op stub, so
the native build has no live ubus stats — that surface only exists on-device.

### Prerequisites

- A C toolchain (`cc`/gcc), `make`, and `python3` (for `make verify`).
- For live capture on a Linux host: `CAP_NET_RAW` (raw `AF_PACKET`).
- For the optional local demo (`demo/demo.sh`): PipeWire with `pw-cli`/`pw-dump`.
- The shared test fixture for `make test`/`make verify` (a captured REAC pcap),
  located via the `REAC_FIXTURE` environment variable.

### Build and test

```sh
make            # builds build/reac-aes67 (default target `all`)
make test       # builds + runs every tests/test_*.c plus tests/test_cli_sdp.sh
make verify     # cross-checks the C decoder against a Python reference decode
make clean      # rm -rf build
```

`make test` exits non-zero and prints `TESTS FAILED` on any failure; otherwise
it prints `ALL TESTS PASSED`. `make verify` runs
`python3 tests/cross_verify.py build/reac-aes67 $REAC_FIXTURE`.

`make` flags (from the Makefile): `CC ?= cc`, `CFLAGS ?= -O2 -Wall -Wextra
-std=c11`, `LDLIBS ?= -lm`. The fixture path defaults to a sibling reac-tools
checkout (`REAC_FIXTURE ?= ../reac-tools/tests/fixtures/real_reac_stream.pcap`);
override it to point at your own capture:

```sh
make test REAC_FIXTURE=<FIXTURE>
```

To cross-compile instead of installing the OpenWrt SDK, prepend a cross prefix
to the compiler with `CROSS` (the value is prepended to `CC`):

```sh
make CROSS=aarch64-openwrt-linux-musl-
```

### Run / smoke-test without REAC hardware

The daemon can synthesize a continuous, real-time-paced multichannel tone and
emit it as AES67, so you can see the stream appear in a local PipeWire receiver
without the REAC rig:

```sh
./demo/demo.sh          # sender + PipeWire receiver; asserts the multichannel node appears
KEEP=1 ./demo/demo.sh   # leave sender + receiver running
```

Under the hood the demo runs the daemon's tone generator and an AES67 receiver
on the same host:

```sh
./build/reac-aes67 --gen-tone --rate 48000 --udp <MCAST_GROUP>:<PORT> --tone-detune 25
```

`--gen-tone` is a continuous sender with no built-in stop; in scripts background
it and kill it (the demo uses a trap). `IP_MULTICAST_LOOP` is forced on, so a
receiver on the same host sees the stream (intended for this local demo). For a
**persistent** PipeWire receiver, drop `demo/reac-a-rtp-source.conf` into
`~/.config/pipewire/pipewire.conf.d/` and restart PipeWire (a `pw-cli
load-module` receiver unloads when `pw-cli` exits).

### Live capture on a Linux host

```sh
./build/reac-aes67 --listen <REAC_IFACE> --udp <MCAST_GROUP>:<PORT> --rate 48000
```

This opens raw `AF_PACKET` capture bound to `ETH_P_REAC` (`0x8819`) on the
interface and needs `CAP_NET_RAW`; on failure it prints
`cannot open capture on <if> (need CAP_NET_RAW?)`. The `--udp` argument is
numeric IPv4 only (`IP:PORT`, port `1..65535`); there is no hostname resolution.
The egress interface is chosen by the kernel routing table for the multicast
group — the daemon does not set `IP_MULTICAST_IF` or bind to a source interface.
