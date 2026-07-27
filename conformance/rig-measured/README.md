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

Everything the tone knob needs is the CURVE: `db[]`, one value per `grid` point, in physical
units on a stated frequency grid. An earlier draft of this fixture also carried a fitted 1-pole
corner (`lp1_hz`, `gain_db`) and a `residual_db` saying how far that fit lay from the truth, so
that a simple player could skip the curve. Measuring real pedals killed it: a Big Muff tone
control is a bass-cut/treble-boost blend of about 30 dB across the band, and the best 1-pole fit
missed it by 15 to 65 dB. Those fields are gone from the format and from this pack; a reader
written against them implements a dead design.

Why this fixture exists separately from `../rig/`: that one pins the SELECTION policy on a sparse
matrix, this one pins the shape a client is actually written against. See `../rig/README.md` for the
three duties, the naming rule (`m01…m03` numbering does not follow settings — names mean nothing),
and regeneration (`NAMZ_REGEN=1`).

The numbers here are hand-authored and plausible, not a real measurement — this is a contract
fixture, not a device model. What must be exact is their round-trip, not their physics.
