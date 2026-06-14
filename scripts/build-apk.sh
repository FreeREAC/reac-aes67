#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# In-container build of whichever FreeREAC OpenWrt packages are present in this
# repo, against the latest stable OpenWrt SDK (or a pinned one). Self-adapting:
# stages + builds only the packages that exist here.
#   OPENWRT_RELEASE  pin a release (default: latest stable)
#   OPENWRT_TARGET   target board (default mediatek/filogic)
set -euo pipefail
REPO=/repo; WORK=/work; OUT_DIR="$WORK/out"
TARGET="${OPENWRT_TARGET:-mediatek/filogic}"
BASE="https://downloads.openwrt.org/releases"

echo "== build deps =="
dnf -q -y install gcc gcc-c++ make git python3 ncurses-devel zlib-devel \
    zstd xz wget curl which unzip file rsync perl gawk diffutils findutils tar bzip2 patch >/dev/null

REL="${OPENWRT_RELEASE:-}"
[ -n "$REL" ] || REL="$(curl -fsSL "$BASE/" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+/' | tr -d '/' | sort -V | tail -1)"
echo "OpenWrt $REL  target $TARGET"
TGT_URL="$BASE/$REL/targets/$TARGET"
SDK_FILE="$(curl -fsSL "$TGT_URL/" | grep -oE 'openwrt-sdk-[^"<]*\.tar\.(xz|zst)' | head -1)"
mkdir -p "$WORK/sdk-cache"; SDK_CACHE="$WORK/sdk-cache/$SDK_FILE"
[ -f "$SDK_CACHE" ] || { curl -fSL --retry 3 -o "$SDK_CACHE.part" "$TGT_URL/$SDK_FILE"; mv "$SDK_CACHE.part" "$SDK_CACHE"; }
SDK_DIR="$WORK/sdk-$REL-$(echo "$TARGET" | tr '/' '-')"
if [ ! -d "$SDK_DIR" ]; then mkdir -p "$SDK_DIR"
  case "$SDK_FILE" in
    *.tar.zst) tar --use-compress-program=unzstd -xf "$SDK_CACHE" -C "$SDK_DIR" --strip-components=1 ;;
    *.tar.xz)  tar -xJf "$SDK_CACHE" -C "$SDK_DIR" --strip-components=1 ;;
  esac
fi
cd "$SDK_DIR"
[ -d feeds/luci ] || { cp feeds.conf.default feeds.conf; ./scripts/feeds update -a >/tmp/feeds.log 2>&1 || { tail -20 /tmp/feeds.log; exit 1; }; ./scripts/feeds install luci-base >/dev/null 2>&1 || true; }

echo "== stage packages present in this repo =="
rm -rf package/reac-* package/luci-app-reac-*
# Purge stale reac apks from the shared SDK bin/ (the cache persists across repo
# builds) so out/ only ever holds packages THIS repo produced this run.
find bin -name '*.apk' -iname '*reac*' -delete 2>/dev/null || true
PKGS=""
if [ -d "$REPO/openwrt/reac-aes67" ] && ls "$REPO"/src/*.c >/dev/null 2>&1; then
  mkdir -p package/reac-aes67/src package/reac-aes67/files
  cp "$REPO/openwrt/reac-aes67/Makefile" package/reac-aes67/Makefile
  cp "$REPO"/src/*.c "$REPO"/src/*.h package/reac-aes67/src/
  cp "$REPO"/openwrt/files/reac-aes67.config "$REPO"/openwrt/files/reac-aes67.init "$REPO"/openwrt/files/capabilities-reac-aes67.json package/reac-aes67/files/
  PKGS="$PKGS reac-aes67"
fi
if [ -d "$REPO/openwrt/reac-repacer" ] && [ -f "$REPO/tools/reac_repacer.c" ]; then
  mkdir -p package/reac-repacer/src package/reac-repacer/files
  cp "$REPO/openwrt/reac-repacer/Makefile" package/reac-repacer/Makefile
  cp "$REPO"/tools/reac_repacer.c package/reac-repacer/src/reac_repacer.c
  sed -i 's/^#define RING_BITS 14 .*/#define RING_BITS 11   \/* PROD ring (patched from test=14 by build-apk.sh) *\//' package/reac-repacer/src/reac_repacer.c
  grep -q '^#define RING_BITS 11' package/reac-repacer/src/reac_repacer.c || { echo "ERROR: RING_BITS patch failed"; exit 1; }
  cp "$REPO"/openwrt/files/reac-repacer.config "$REPO"/openwrt/files/reac-repacer.init package/reac-repacer/files/
  cp "$REPO"/openwrt/reac-repacer/files/* package/reac-repacer/files/
  PKGS="$PKGS reac-repacer"
fi
if [ -d "$REPO/openwrt/reac-transport" ]; then
  mkdir -p package/reac-transport
  cp "$REPO/openwrt/reac-transport/Makefile" package/reac-transport/Makefile
  cp -r "$REPO"/openwrt/reac-transport/files package/reac-transport/files
  PKGS="$PKGS reac-transport"
fi
for la in luci-app-reac-aes67 luci-app-reac-repacer; do
  if [ -d "$REPO/openwrt/$la" ]; then mkdir -p package/$la; cp -r "$REPO"/openwrt/$la/* package/$la/; PKGS="$PKGS $la"; fi
done
echo "staging:$PKGS"
[ -n "$PKGS" ] || { echo "ERROR: no reac packages found in $REPO"; exit 1; }

echo "== configure =="
make defconfig >/dev/null 2>&1
for p in $PKGS; do echo "CONFIG_PACKAGE_$p=y"; done >> .config
make defconfig >/dev/null 2>&1

for pkg in $PKGS; do
  echo "== compile $pkg =="
  make "package/$pkg/compile" V=s -j"$(nproc)" 2>&1 | tee "/tmp/build-$pkg.log" | tail -4 || { echo "--- $pkg tail ---"; tail -40 "/tmp/build-$pkg.log"; exit 1; }
done

echo "== artifacts =="
mkdir -p "$OUT_DIR"; rm -f "$OUT_DIR"/*.apk
# Copy only the apks for the packages this repo staged + built (scoped to $PKGS),
# never a blanket reac glob — keeps each repo's release free of cross-repo apks.
for pkg in $PKGS; do find bin -name "${pkg}-*.apk" -exec cp {} "$OUT_DIR/" \; ; done
ls -la "$OUT_DIR/"; echo "$REL" > "$OUT_DIR/.openwrt-release"
