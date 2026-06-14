#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# Portions derived from obs-h8819-source, Copyright (C) 2022 Norihiro Kamae <norihiro@nagater.net> (GPL-3.0-or-later).

"""Cross-validate the C decoder against an independent Python decode of the
SAME real capture. Two implementations agreeing on the real bytes is far
stronger than either alone.

The Python side de-interleaves the audio region itself, straight from the pcap
frames — its own parser, extraction and sign-extension, no C code — then
compares against `reac-aes67 --dump-samples`. It uses the M-5000's plain
sequential little-endian layout (channel ch / time-sample s at byte
(s*n_ch + ch)*3), the same layout reac_decode.c targets. This is DELIBERATELY
not obs-h8819's braided convert_to_pcm24lep (even = bytes 3,0,1; odd = 4,5,2):
that braid is faithful to obs-h8819's device but scrambles the M-5000's payload
into noise (verified on-rig 2026-06-06 — plain LE: coherence 0.999; braid:
noise). The cross-check still guards the C decoder against offset, endianness
and sign-extension regressions.

Usage: cross_verify.py <reac-aes67-binary> <fixture.pcap>
Exit 0 if every sample of every frame matches; nonzero + diff otherwise.
"""
import subprocess
import sys
import struct


def py_decode_frames(pcap_path, n_ch=40, ns=12):
    """Yield (seq, [signed24 samples channel-major]) per REAC frame in the pcap."""
    with open(pcap_path, "rb") as f:
        gh = f.read(24)
        magic = struct.unpack("<I", gh[:4])[0]
        le = magic == 0xA1B2C3D4
        endp = "<" if le else ">"
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                break
            _sec, _usec, incl, _orig = struct.unpack(endp + "IIII", ph)
            raw = f.read(incl)
            if len(raw) < incl:
                break
            if raw[12] != 0x88 or raw[13] != 0x19:
                continue
            seq = raw[14] | (raw[15] << 8)  # l2_counter LE
            audio = raw[50:50 + n_ch * ns * 3]
            samples = []
            for ch in range(n_ch):
                for s in range(ns):
                    sp = (s * n_ch + ch) * 3  # plain sequential LE (M-5000 layout)
                    v = audio[sp] | (audio[sp + 1] << 8) | (audio[sp + 2] << 16)
                    if v & 0x800000:
                        v -= 1 << 24
                    samples.append(v)
            yield seq, samples


def c_decode_frames(binary, pcap_path):
    out = subprocess.run([binary, "--pcap", pcap_path, "--dump-samples"],
                         capture_output=True, text=True, check=True).stdout
    frames = []
    for line in out.strip().splitlines():
        # "frame N seq S: v v v ..."
        head, _, rest = line.partition(":")
        parts = head.split()
        seq = int(parts[3])
        samples = [int(x) for x in rest.split()] if rest.strip() else []
        frames.append((seq, samples))
    return frames


def main():
    if len(sys.argv) != 3:
        print("usage: cross_verify.py <binary> <fixture.pcap>", file=sys.stderr)
        return 2
    binary, pcap = sys.argv[1], sys.argv[2]
    py = list(py_decode_frames(pcap))
    c = c_decode_frames(binary, pcap)
    if len(py) != len(c):
        print(f"FAIL: frame count C={len(c)} Python={len(py)}")
        return 1
    for i, ((pseq, ps), (cseq, cs)) in enumerate(zip(py, c)):
        if pseq != cseq:
            print(f"FAIL frame {i}: seq C={cseq} Python={pseq}")
            return 1
        if ps != cs:
            # find first mismatch
            for j, (a, b) in enumerate(zip(ps, cs)):
                if a != b:
                    print(f"FAIL frame {i} sample {j}: C={cs[j]} Python={ps[j]}")
                    return 1
            print(f"FAIL frame {i}: length C={len(cs)} Python={len(ps)}")
            return 1
    nsamp = sum(len(s) for _, s in py)
    print(f"OK: {len(py)} frames, {nsamp} samples — C decoder matches Python reference exactly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
