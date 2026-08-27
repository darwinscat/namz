<!-- SPDX-License-Identifier: MIT -->
# Rig conformance — the golden `.orbitrig` packs

The `.orbitrig` contract has TWO ends — a capture tool that **writes** packs (OrbitNamCapture) and a
player that **reads** them — living in different repositories. These fixtures are the one committed
truth both ends test against, so the format can never drift silently: any change that breaks the
contract fails a CI **at PR time**, in whichever repo made it.

Two fixtures, one reference test:

```
rig/                  a sparse channel × gain matrix — pins the SELECTION policy
rig-measured/         one gain dial + two TONE (linear) knobs, one per form — the minimal modern device
  pack/               the golden pack, folder shape (zip it for the exchange-shape tests)
    rig.json          written by the CANONICAL writer (namz::rig::writeManifest) — byte-exact truth
    m01.namz          tiny models with stamped Capture-identity keys (namz::rig::stampMeta vocabulary)
  expected.json       machine-readable expectations: rig identity, the device, a selection table
```

`rig/` is deliberately SPARSE (red at 150° was never captured; 300° is declared on the dial but no
file carries it) so the selection table pins the fallback policy, not just the happy path.

The models are named `m01…m03`, and the numbering deliberately does NOT follow the settings — in
`rig/`, `m01` is the RED file and `m02` is green at 150°. **Filenames carry no information in this
format**, and a fixture is the wrong place to imply otherwise: a reader that infers settings from a
name now fails the selection table here, instead of failing in someone's session six months later.

## What each side must check

**The reference implementation** (`tests/rig_conformance.cpp`, runs in this repo's CI):
1. read — `loadRig(rig.json, per-file readMeta)` reproduces `expected.json` (identity, gear,
   controls spec, dial sweeps, tone block, file index), and the manifest-less fallback
   (`loadRigFromFiles`) builds the SAME device from the headers alone (spec decision A: every
   `.namz` is self-sufficient — minus `tone`, which is device-scoped and manifest-only).
2. select — every `selection` row resolves to its `expect` (null = no file, selection unchanged).
3. write — rebuilding the model from `expected.json` and running it through `writeManifest` /
   `stampMeta` reproduces `rig.json` and every file's header **byte-for-byte**.

**A capture tool** (writer): build this rig in its own model, export a pack — the manifest and each
file's metadata must byte-match `pack/`. If it uses `namz_rig_write.h` this holds by construction;
the check catches accidental bypasses.

**A player** (reader): load `pack/` (and its zipped form) — the device, controls and the selection
table must match `expected.json`, and the tone knobs must be applied as DSP, never offered as a
model-selection axis.

Regeneration: the reference test **is** the recipe — run it with `NAMZ_REGEN=1` and it writes every
`pack/` from `expected.json` (`namz::pack` over `vectors/flat.nam` + `stampMeta`, and `writeManifest`
for the manifest). The bytes are committed; regenerating an unchanged fixture must be a no-op, which
is exactly what duty 3 asserts.
