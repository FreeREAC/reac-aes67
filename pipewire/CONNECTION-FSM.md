# Connection FSM (JOIN/HOLD) — not in this public repo

This `pipewire/` tree is the public-grade PipeWire-native REAC endpoint: it
RX-decodes the master's downstream broadcast into PipeWire graph nodes
(`reac:capture`) and TX-encodes graph audio back to REAC (`reac:playback`),
sharing the reac-aes67 decode core in `../src`.

The **virtual-stagebox JOIN/HOLD connection FSM** — the control-plane builders
(`reac_ctrl`) and the state machine (`reac_fsm`) that make a real Roland master
*grant and hold* a link — is **deliberately not included here.** It is derived
from firmware RE + the gold cold-connect captures and is governed by the §4.2
rule (private RE only). It lives in:

- `reac-pw` (private working repo) — `src/reac_ctrl.{c,h}`, `src/reac_fsm.{c,h}`
  with offline tests.
- `reac-firmware-re/REAC-CONNECTION-FSM.md` — the consolidated spec, evidence
  grades, blocking gaps, and the rig-capture playbook.

Building the stagebox (active-speaker) variant is a private build that adds
those modules; do not vendor them into this public repo without an explicit
sanitization decision.
