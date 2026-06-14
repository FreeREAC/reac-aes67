#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

# CLI smoke: reac-aes67 --print-sdp emits a valid SDP for the configured stream.
# Run after `make`. Returns 0 on pass.
set -e
BIN="${1:-build/reac-aes67}"
out=$("$BIN" --print-sdp --name REAC-A --rate 48000 --udp 239.69.0.1:5004 --pt 97 --ssrc 11223344 --origin 10.0.0.1)
echo "$out"
echo "$out" | grep -q "^v=0" || { echo "FAIL: no v=0"; exit 1; }
echo "$out" | grep -q "^s=REAC-A" || { echo "FAIL: no session name"; exit 1; }
echo "$out" | grep -q "m=audio 5004 RTP/AVP 97" || { echo "FAIL: no m=audio line"; exit 1; }
echo "$out" | grep -q "a=rtpmap:97 L24/48000/40" || { echo "FAIL: no rtpmap L24/48000/40"; exit 1; }
echo "$out" | grep -q "c=IN IP4 239.69.0.1/1" || { echo "FAIL: no connection line"; exit 1; }
# plain AES67 SDP must NOT carry RFC 7273 clock lines
echo "$out" | grep -q "ts-refclk" && { echo "FAIL: AES67 SDP has ts-refclk"; exit 1; }

# --- Dante-interop profile: per-flow RFC 7273 SDP ---
dout=$("$BIN" --print-sdp --profile dante --name REAC-A --origin 10.0.0.1 \
       --aes67-base 239.69.10.0 --gmid 00-11-22-FF-FE-33-44-55)
echo "$dout" | grep -q "a=rtpmap:96 L24/48000/8" || { echo "FAIL: dante rtpmap"; exit 1; }
echo "$dout" | grep -q "a=ptime:1" || { echo "FAIL: dante ptime"; exit 1; }
echo "$dout" | grep -q "a=ts-refclk:ptp=IEEE1588-2008:00-11-22-FF-FE-33-44-55:0" || { echo "FAIL: dante ts-refclk"; exit 1; }
echo "$dout" | grep -q "a=mediaclk:direct=0" || { echo "FAIL: dante mediaclk"; exit 1; }
# 40 ch / 8 = 5 flows on 239.69.10.0..4
echo "$dout" | grep -q "c=IN IP4 239.69.10.0/" || { echo "FAIL: dante flow 0 addr"; exit 1; }
echo "$dout" | grep -q "c=IN IP4 239.69.10.4/" || { echo "FAIL: dante flow 4 addr"; exit 1; }
nflows=$(echo "$dout" | grep -c "^m=audio")
[ "$nflows" = "5" ] || { echo "FAIL: expected 5 dante flows, got $nflows"; exit 1; }

# --- Dante packetizer chain: 4 fixture frames @ FPP=4 -> 1 window x 5 flows = 5 ---
FIX="${REAC_FIXTURE:-tests/fixtures/real_reac_stream.pcap}"
if [ -f "$FIX" ]; then
	cnt=$("$BIN" --count-rtp --profile dante --pcap "$FIX" 2>/dev/null)
	echo "$cnt" | grep -q "rtp_packets 5 flows 5" || { echo "FAIL: dante count ($cnt)"; exit 1; }
fi
echo "CLI-SDP OK"
