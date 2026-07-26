<!-- SPDX-License-Identifier: MIT -->
# `rig-measured` — the minimal modern device

The second golden pack: **one gain dial + one measured knob**. It is the smallest device shape a
player must handle end to end, and it is deliberately the shape a real dirt pedal has.

```
Golden Drive        pedal, tone_type "crunch"
  gain              a DIAL: sweep 300°, captured at 0 / 150 / 300 → three models
  tone              MEASURED: linear, never captured — six swept positions shipped as a filter
```

A player does two different things with those two knobs:

- **gain** is in `controls` — turning it SELECTS a file (`m01…m03`);
- **tone** is in `measured` — turning it changes nothing about which file plays; the player builds a
  filter from `positions[]` and applies it after the stage. Interpolating between positions is what
  makes it a continuous knob out of six measurements.

Everything the tone knob needs is in physical units (`lp1_hz`, `gain_db`), so it works at any sample
rate, plus the raw `db` curve as provenance — a better filter design can ship later without touching
the pack. `residual_db` says how far the cheap 1-pole fit is from that curve.

Why this fixture exists separately from `../rig/`: that one pins the SELECTION policy on a sparse
matrix, this one pins the shape a client is actually written against. See `../rig/README.md` for the
three duties, the naming rule (`m01…m03` numbering does not follow settings — names mean nothing),
and regeneration (`NAMZ_REGEN=1`).

The numbers here are hand-authored and plausible, not a real measurement — this is a contract
fixture, not a device model. What must be exact is their round-trip, not their physics.
