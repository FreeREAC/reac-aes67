<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
-->

# AES/EBU (AES3) ↔ REAC conversion — design notes for a future reac-aes67 profile

**Status:** DESIGN / reference. Off-hardware so far; the AES3 framing logic is
unit-testable, but live lock against real AES/EBU gear is rig-gated.
**Scope:** how a Roland-V-mixer-class console exposes AES/EBU (AES3) input/output
alongside REAC, and what `reac-aes67` would need to add to grow an **AES/EBU
profile** (`--profile aes-ebu`) that either (a) emits the REAC audio it already
decodes as AES3, or (b) accepts AES3 and re-emits it as REAC/AES67. Sibling of
the [AES67 bridge design](2026-05-30-reac-aes67-bridge-design.md) and the
[Dante-interop profile](2026-06-03-reac-dante-aes67-interop-design.md); it reuses
their decode / media-clock / PLC core unchanged.

> This document is written from **published vendor documentation** (V-mixer
> owner's references) and the **public AES3 / IEC 60958 standards**. It contains
> no device-specific addresses, no network identifiers, and no site channel names.

---

## 1. The headline facts about console-side AES/EBU

A V-mixer-class console of this generation carries AES/EBU as **two stereo AES3
pairs** in each direction, on the rear panel:

- `AES/EBU IN 1/2`, `AES/EBU IN 3/4`
- `AES/EBU OUT 1/2`, `AES/EBU OUT 3/4`

All four are **IEC 60958-compliant** (AES3 professional / IEC 60958 type I on XLR,
or the consumer variant on coax depending on the jack). Each pair is one AES3
stream whose two subframes (A/B) carry the L/R channels of the pair.

Three properties dominate the whole design and are **confirmed in the vendor
documentation**:

1. **24-bit PCM throughout.** Internal mixing, REAC transport and AES3 all run at
   24-bit. This matches `reac-aes67`'s existing REAC decode (3 bytes/sample) and
   its AES67 `L24` sender — no width conversion is ever required on this path.
2. **No sample-rate converter on the AES/EBU inputs.** The vendor explicitly
   states the AES/EBU IN jacks are **not equipped with an SRC**; an incoming AES3
   stream **must already be synchronous** to the console's word clock. This makes
   the whole conversion **synchronous by construction** (see §4).
3. **One global sample rate.** The console runs its entire engine (AD/DA, mixing,
   FX, REAC, AES) at a single selected rate: **96 kHz, 48 kHz or 44.1 kHz**. There
   is no per-port rate. 48 k and 96 k are the common operational rates; 44.1 k is
   supported but seldom used in this class of system.

A reverse-engineering pass on the console firmware corroborates all three (24-bit
ALSA format, IEC958 channel-status `#define`s for 24-bit/professional, the
absence of any per-AES SRC symbol, and the same {44.1, 48, 96} kHz rate set), and
adds two structural facts that inform the design:

- The REAC↔AES3 reframing crossbar is implemented in the console's **FPGA**, not
  in CPU code. The CPU only **configures** it through a memory-mapped FPGA
  register block (a per-REAC-port array of 16-bit words). For `reac-aes67` this is
  irrelevant to *transport* but relevant to the *mental model*: on the real desk,
  "which source feeds AES OUT 1/2" is a **patchbay/routing parameter**, identical
  in nature to "which source feeds a REAC out slot."
- The console maintains a full **AES3 channel-status array** (24 bytes) and can,
  internally, lock its clock to an AES input. (The vendor UI does **not** expose
  AES-as-master — AES IN is documented as a clock *slave*. Treat AES-as-master as
  an undocumented internal capability, not a feature to rely on.)

> Nothing above is unique to one unit — it is the documented behaviour of the
> product family. The spec deliberately does not pin a firmware version.

---

## 2. Where an AES/EBU profile sits relative to REAC

```
        REAC 0x8819 frames                         AES3 / IEC 60958 stream(s)
                │                                            │
   ┌────────────┴───────────┐                  ┌─────────────┴─────────────┐
   │ EXISTING reac-aes67 core│                  │  NEW: aes3 codec (this    │
   │ reac_decode → media_clk │ <── planar PCM ──>│  profile): pack/unpack    │
   │ → PLC (N ch, M samp,    │   (24-bit, per-   │  AES3 subframes + channel-│
   │  rate R, planar LE)     │    frame planar)  │  status; sync gate        │
   └────────────┬───────────┘                  └─────────────┬─────────────┘
                │                                            │
        AES67 / Dante sender                       AES3 hardware framer
        (existing)                                 (sound card / FPGA / dev)
```

The conversion is **purely a framing + clocking concern**, never a DSP concern,
because both sides are 24-bit PCM at the same rate. The new code is:

1. an **AES3 subframe codec** (pure, unit-testable): pack a pair of 24-bit
   samples + their channel-status/validity/parity bits into AES3 subframes, and
   the inverse;
2. a **channel-status manager**: build/parse the 24-byte AES3 channel-status block
   (professional bit, FS bits, word length = 24-bit, etc.);
3. a **synchronous gate**: refuse/flag any path where the two clock domains are
   not provably locked (because there is no SRC).

Everything else (decode, media clock, PLC, RTP/SDP) is reused.

---

## 3. Channel mapping (REAC slots ⇄ AES3 pairs)

REAC carries N planar channels per frame (the existing descriptors:
`REAC_MODE_*` for {rate, channels, samples}). AES3 carries channels in **stereo
pairs**. The mapping is a trivial deterministic interleave/de-interleave; the
*selection* of which REAC slot feeds which AES pair is a routing decision (a
config item, mirroring the console's patchbay), never hard-coded.

```
AES pair k  (subframe A, subframe B)  ⇄  REAC slots (2k-1, 2k)   [default 1:1]
```

- **REAC → AES3** (emit): for each configured AES pair, take the two selected
  REAC slot sample-streams (already de-interleaved by `reac_decode`) and pack them
  into subframes A/B. M samples per REAC frame → M AES3 frames.
- **AES3 → REAC** (ingest): de-interleave subframes A/B into two slot streams,
  drop them into the planar buffer the encoder/sender already consumes.

Pair count: a console of this class exposes **2 stereo pairs** (4 channels) of
AES/EBU each way. The profile config should let the operator choose any subset of
decoded REAC slots to route to AES OUT 1/2 and 3/4 (and the reverse), exactly as
the desk's OUTPUT/INPUT patchbay tabs do. **No fixed slot numbers are baked in.**

---

## 4. Clocking — the load-bearing constraint

Because **there is no SRC**, AES3 and REAC must share a clock. The design must
make synchronicity an explicit, checked precondition, not an assumption.

### 4.1 Direction REAC → AES3 (we generate AES3)

The REAC stream is the clock master (its 0x8819 sample tick). The AES3 framer
must be clocked **from the same domain**:

- If a sound card / interface frames the AES3, it must be slaved to the REAC-
  derived word clock (external WORD CLOCK, or the card locked to the recovered
  REAC rate). `reac-aes67`'s media clock already recovers the REAC rate; the
  profile exports it as the AES3 frame rate and **must not free-run** the framer.
- Rate is fixed per session to one of {44100, 48000, 96000} and must equal the
  decoded `REAC_MODE_*` rate. A mismatch is a hard configuration error.

### 4.2 Direction AES3 → REAC/AES67 (we consume AES3)

Per the vendor rule, the **incoming AES3 must already be word-clock-locked** to
the system. The profile therefore:

- reads the AES3 **lock / FS status** from the framer before accepting audio;
- if the source is not locked (or FS ≠ system FS), it **mutes and flags** rather
  than resampling — there is deliberately no SRC, to match the desk's behaviour
  and avoid hidden pitch/artefact errors;
- AES-as-master clock is **not** offered as a supported mode (undocumented on the
  desk); the supported masters mirror the desk's documented set: INTERNAL, WORD
  CLOCK, REAC A, REAC B (plus expansion-slot sources, out of scope here).

### 4.3 Why this is safe to unit-test off-hardware

The subframe codec and channel-status manager are pure functions over byte
buffers. The clocking gate is a small state machine over a "locked?/FS" input.
Both can be exhaustively tested with fixtures; only the *physical* lock needs a
rig. This mirrors how the AES67/Dante profiles are validated.

---

## 5. AES3 / IEC 60958 framing the codec must implement

Standard AES3, no vendor extensions:

- **Subframe** (32 time slots): 4-bit preamble (X/Y/Z), 24-bit audio (LSB-first,
  slots 4-27; a 24-bit word uses all of them), then **V** (validity), **U** (user),
  **C** (channel-status), **P** (parity, even over slots 4-31).
- **Frame** = 2 subframes (channel A then B). **Block** = 192 frames; the C bits
  across a block assemble the **24-byte channel-status** array.
- Preambles: **Z** marks the start of a channel-status block (frame 0, subframe
  A), **X** = subframe A otherwise, **Y** = subframe B.
- **Channel-status** the manager sets for an output (professional / IEC 60958
  type I): professional bit = 1, audio (non-data), no emphasis, **sampling-
  frequency bits** = the session FS, **word length** = 24-bit. On ingest it should
  *read* FS + word-length and cross-check against the session, and surface
  validity (V).

Because the actual sample-level slot/preamble/biphase-mark encoding is what real
hardware (a sound card S/PDIF-AES port, an FPGA, or a USB-AES interface) does in
silicon, `reac-aes67`'s software layer typically stops at **PCM + channel-status
metadata** and hands raw 24-bit frames to the framer (e.g. ALSA `S24_3LE`,
matching REAC's 3-byte samples). The full bit-level codec in §5 is needed only
for a pure-software framer or for test fixtures.

---

## 6. Config surface (proposed)

UCI-style, consistent with the existing daemon:

```
config aes_ebu 'profile'
    option enabled       '0'
    option direction     'reac_to_aes'   # or 'aes_to_reac'
    option rate          '48000'         # MUST equal decoded REAC rate
    option require_lock  '1'             # mute+flag if source not word-clock-locked
    # routing: which decoded REAC slots feed each AES pair (1-based slot indices);
    # left intentionally as operator config — mirrors the desk patchbay, no defaults baked in
    option pair12_a      ''              # REAC slot -> AES OUT 1 (subframe A)
    option pair12_b      ''              # REAC slot -> AES OUT 2 (subframe B)
    option pair34_a      ''
    option pair34_b      ''
```

`ubus` status should expose: session FS, per-pair lock state, channel-status FS +
word-length read back from any AES input, and a mute/flag counter for unlocked
input — paralleling the desk's "STATUS shows un-synced connectors in red."

---

## 7. Non-goals / explicit limits

- **No sample-rate conversion.** By design, matching the hardware. Cross-rate
  bridging is a different, future component, not this profile.
- **No AES-as-master clocking.** Undocumented on the reference hardware; excluded.
- **No console control.** This profile only moves audio; it does not read or write
  the desk's patchbay. (The desk's routing parameters live on a separate internal
  control plane and are not part of any documented audio-bridge.)
- **2 stereo pairs each way**, matching the documented rear-panel jack count;
  expansion-slot AES is out of scope.

---

## 8. Test plan (off-hardware first)

1. **Subframe codec round-trip:** random 24-bit sample pairs → AES3 subframes →
   back; assert bit-exact incl. parity.
2. **Channel-status block:** build a 192-frame block, assert the 24-byte array
   decodes to {professional, 24-bit, FS} correctly for each of 44.1/48/96 k.
3. **Interleave mapping:** N planar REAC slots ⇄ AES pairs, assert deterministic
   and inverse-correct for several routings.
4. **Sync gate:** drive the lock/FS state machine with locked / unlocked / wrong-
   FS inputs; assert mute+flag on the bad cases, pass-through on the good case.
5. **Rig-gated:** real AES3 source locked to system word clock → REAC/AES67 out,
   listen + meter; then REAC in → AES3 out into known-good AES gear.

---

## 9. Relationship to the rest of reac-aes67

- Reuses: `reac_decode`, `media_clock`, PLC, the planar PCM buffer contract, and
  (for the AES→AES67 path) the existing RTP `L24` sender + SDP.
- Adds: `aes3_codec` (pure), `aes3_channel_status` (pure), `aes_sync_gate` (small
  FSM), and a thin framer adapter (ALSA `S24_3LE` to start).
- Profiles are siblings: `--profile aes67` (default), `--profile dante`,
  `--profile aes-ebu` (this one). They differ only in the **egress/ingress
  framing + clocking**, never in the decode core.
