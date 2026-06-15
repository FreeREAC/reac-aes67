#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble a self-contained reac-aes67 source tarball for rpmbuild: vendors
# libreac (the Makefile compiles libreac/src/reac.c in directly). Writes
# reac-aes67-<version>.tar.gz to the repo root. Override the libreac checkout
# with LIBREAC=/path/to/libreac.
set -e
V="${1:-0.3.0}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIBREAC="${LIBREAC:-$ROOT/../reac-pw/subprojects/libreac}"
[ -f "$LIBREAC/src/reac.c" ] || { echo "libreac not found at $LIBREAC (set LIBREAC=)"; exit 1; }

T=$(mktemp -d); D="$T/reac-aes67-$V"; mkdir -p "$D"
rsync -a --exclude '.git' --exclude 'build' --exclude '.build' \
      "$ROOT/src" "$ROOT/tests" "$ROOT/Makefile" "$ROOT/LICENSE" "$ROOT/README.md" \
      "$ROOT/packaging" "$D/"
mkdir -p "$D/third_party/libreac"
rsync -a --exclude '.git' --exclude 'build' "$LIBREAC/src" "$LIBREAC/include" "$D/third_party/libreac/"
tar -czf "$ROOT/reac-aes67-$V.tar.gz" -C "$T" "reac-aes67-$V"
rm -rf "$T"
echo "wrote $ROOT/reac-aes67-$V.tar.gz"
