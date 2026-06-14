#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

# End-to-end local demo: simulate a running reac-aes67 daemon streaming a
# synthetic 40-channel tone as AES67, receive it in PipeWire, and prove a
# 40-channel node appears (and optionally record a few seconds).
#
#   ./demo/demo.sh            # run the demo, auto-cleanup
#   KEEP=1 ./demo/demo.sh     # leave sender + receiver running (Ctrl-C to stop)
#
# Requires: built ./build/reac-aes67, PipeWire with module-rtp-source, pw-dump.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$HERE/build/reac-aes67"
GROUP=239.69.0.1; PORT=5004

[ -x "$BIN" ] || { echo "build first: make"; exit 1; }

echo "== 1. start the tone sender (40 ch @ 48 kHz, +25 Hz/ch, AES67 multicast) =="
"$BIN" --gen-tone --rate 48000 --udp "$GROUP:$PORT" --tone-detune 25 >/tmp/reac-gen.log 2>&1 &
GEN=$!
trap 'kill $GEN 2>/dev/null; [ -n "${PWPID:-}" ] && kill $PWPID 2>/dev/null' EXIT
sleep 0.5
kill -0 $GEN 2>/dev/null || { echo "sender died:"; cat /tmp/reac-gen.log; exit 1; }
echo "   sender pid $GEN"

echo "== 2. load the PipeWire RTP receiver (held open) =="
( printf 'load-module libpipewire-module-rtp-source source.ip=%s source.port=%s sess.latency.msec=100 audio.format=S24BE audio.rate=48000 audio.channels=40\n' "$GROUP" "$PORT"
  sleep "${HOLD:-8}" ) | pw-cli >/tmp/reac-pwcli.log 2>&1 &
PWPID=$!
sleep 2

echo "== 3. is the 40-channel node on the graph? =="
pw-dump 2>/dev/null | python3 -c "
import json,sys
found=False
for o in json.load(sys.stdin):
    if o.get('type','').endswith('Node'):
        p=o.get('info',{}).get('props',{})
        ch=int(p.get('audio.channels',0) or 0)
        n=p.get('node.name','') or ''
        if 'rtp' in n.lower() and ch>=40:
            print('   NODE %r  channels=%d  class=%s' % (n, ch, p.get('media.class')))
            found=True
sys.exit(0 if found else 1)
" && echo "   DEMO OK: 40-channel AES67 stream visible on the network/graph" \
  || { echo '   node not found — see /tmp/reac-pwcli.log'; cat /tmp/reac-pwcli.log; }

if [ "${KEEP:-0}" = "1" ]; then
  echo "== KEEP=1: leaving sender + receiver running. Ctrl-C to stop. =="
  echo "   Try: wpctl status | grep -i reac   ·   qpwgraph   ·   pw-record --target reac-a-rtp /tmp/reac.wav"
  trap - EXIT
  wait $GEN
fi
