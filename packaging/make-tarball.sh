#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble a reac-aes67 source tarball for rpmbuild. The Fedora build links the
# system libreac (make LIBREAC_SYSTEM=1, BuildRequires: libreac-devel), so no
# vendoring is needed. Writes reac-aes67-<version>.tar.gz to the repo root.
set -e
V="${1:-0.3.0}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)

T=$(mktemp -d); D="$T/reac-aes67-$V"; mkdir -p "$D"
rsync -a --exclude '.git' --exclude 'build' --exclude '.build' \
      "$ROOT/src" "$ROOT/tests" "$ROOT/Makefile" "$ROOT/LICENSE" "$ROOT/README.md" \
      "$ROOT/packaging" "$D/"
tar -czf "$ROOT/reac-aes67-$V.tar.gz" -C "$T" "reac-aes67-$V"
rm -rf "$T"
echo "wrote $ROOT/reac-aes67-$V.tar.gz"
