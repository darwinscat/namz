<!-- SPDX-License-Identifier: MIT -->
# Rig conformance — the golden `.orbitrig` pack

The `.orbitrig` contract has TWO ends — a capture tool that **writes** packs (OrbitNamCapture) and a
player that **reads** them (OrbitCab) — living in different repositories. These fixtures are the one
committed truth both ends test against, so the format can never drift silently: any change that
breaks the contract fails a CI **at PR time**, in whichever repo made it.

```
pack/                 the golden pack, folder shape (zip it for the exchange-shape tests)
  rig.json            written by the CANONICAL writer (namz::rig::writeManifest) — byte-exact truth
  Golden-*.namz       tiny models with stamped Capture-identity keys (namz::rig::stampMeta vocabulary)
expected.json         machine-readable expectations: rig identity, the device, a selection table
```

The pack is deliberately SPARSE (red-12h was "never captured", 17h is declared but absent) so the
selection table pins the fallback policy, not just the happy path.

## What each side must check

**The reference implementation** (`tests/rig_conformance.cpp`, runs in this repo's CI):
1. read — `loadRig(rig.json, per-file readMeta)` reproduces `expected.json` (identity, gear,
   controls spec, file index), and the manifest-less fallback (`loadRigFromFiles`) builds the SAME
   device from the headers alone (spec decision A: every `.namz` is self-sufficient).
2. select — every `selection` row resolves to its `expect` (null = no file, selection unchanged).
3. write — rebuilding the model from `expected.json` and running it through `writeManifest` /
   `stampMeta` reproduces `rig.json` and every file's header **byte-for-byte**.

**A capture tool** (writer): build this rig in its own model, export a pack — the manifest and each
file's metadata must byte-match `pack/`. If it uses `namz_rig_write.h` this holds by construction;
the check catches accidental bypasses.

**A player** (reader): load `pack/` (and its zipped form) — the device, controls and the selection
table must match `expected.json`.

Regeneration: the `.namz` come from `namz encode conformance/vectors/flat.nam --set …` (stamps as in
`expected.json`), `rig.json` from `writeManifest` over the same model — see the reference test,
which IS the regeneration recipe. The bytes are committed; regenerating must be a no-op.
