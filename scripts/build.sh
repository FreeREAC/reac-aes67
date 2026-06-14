#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Build the reac-aes67 OpenWrt .apk packages against the LATEST stable OpenWrt
# release (or a pinned one) in a Fedora container. The apks land in .build/out/.
#
#   ./build/build.sh                           # latest stable, mediatek/filogic (GL-MT6000)
#   OPENWRT_RELEASE=24.10.2 ./build/build.sh   # pin a release
#   OPENWRT_TARGET=ramips/mt7621 ./build/build.sh
#   CONTAINER_ENGINE=docker ./build/build.sh   # use docker instead of podman
#
# Requires only podman (or docker). The OpenWrt SDK is downloaded once and cached
# under .build/sdk-cache/ (gitignored). Reproducible: same release+target -> same apks.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$REPO/.build"
ENGINE="${CONTAINER_ENGINE:-podman}"
IMAGE="${BUILD_IMAGE:-docker.io/library/fedora:42}"
mkdir -p "$WORK/out"

command -v "$ENGINE" >/dev/null 2>&1 || { echo "ERROR: '$ENGINE' not found (set CONTAINER_ENGINE)"; exit 1; }

echo "Building reac apks — OpenWrt ${OPENWRT_RELEASE:-latest stable}, target ${OPENWRT_TARGET:-mediatek/filogic}, via $ENGINE"
# --security-opt label=disable: required on SELinux hosts so the bind mounts are readable.
"$ENGINE" run --rm --security-opt label=disable \
    -v "$WORK":/work -v "$REPO":/repo:ro \
    -e OPENWRT_RELEASE="${OPENWRT_RELEASE:-}" \
    -e OPENWRT_TARGET="${OPENWRT_TARGET:-}" \
    "$IMAGE" \
    bash /repo/scripts/build-apk.sh

echo
echo "== built apks (.build/out/) =="
ls -1 "$WORK/out/"*.apk 2>/dev/null || { echo "(none — build failed)"; exit 1; }
[ -f "$WORK/out/.openwrt-release" ] && echo "OpenWrt release: $(cat "$WORK/out/.openwrt-release")"
