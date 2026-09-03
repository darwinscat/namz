<!-- SPDX-License-Identifier: MIT -->
# Changelog

All notable changes to **namz** are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project uses
[Semantic Versioning](https://semver.org/). The C++ reference versions independently; each language port
carries its own version.

## [4.0.0] — 2026-09-03 — a switch has an order, not an angle

Breaking: the rig manifest's `schema` is now **4**. A schema-3 reader meets a knob shipping bands per
position, finds no `grid`, never reads its positions and drops the knob — the author declared bands and
the pack would play the reference, silently. That is the failure the number exists to catch.

### Added
- **`positions[].sections`** — a knob that CLICKS, stating each filter. A switch's positions are named,
  ordered, and have nothing in between them, so there is no rotation for a band's gain to travel on:
  each position states the filter it IS (`type`, `hz`, `q`, `gain_db`, and `pivot` on a tilt), and
  nothing is computed between two of them. The reference position ships `"sections": []` — spelled out,
  because "flat here" is a statement and a missing key is an omission, and the two must not look alike.
  Switch-only: a dial has a travel law, and two laws for one control is two products.
  - The travel keys `range_db` / `hz_at` / `q_at` are **forbidden** inside a position's band and the
    manifest is refused over them, rather than read with the key ignored: a reader that ignored one
    would play a fixed band where the producer thought it wrote a travelling one.
  - A band this reader cannot build drops the **knob**, never one band — half a pedal is a different
    pedal, while the reference position is what dropping a tone knob has always meant.
  - Between two positions the CHAIN may change shape, so a player must not ramp band *k*'s coefficients
    from one position's to the next's: build the new cascade beside the old one and cross-fade the output.

### Changed
- **`norm` exists only where rotation exists.** A switch has an order and no angle; until now it shipped
  evenly spaced steps — `i/(n−1)`, a number nobody measured — in a field that promises a measurement.
  A switch now carries none, its array order is the panel's order, and a position is addressed by its
  `value`, never by its place in the array. `norm` present without a `sweep`, or absent with one, is an
  **invalid manifest**: absent and `0.0` are one value in a reader's number, so the rule cannot be a
  convention. The same test catches a `"sweep": 300.0` typo, which used to read as no sweep at all and
  turn a 300° dial into a stepper without a word. Applies to `blend` positions identically.
- **A switch blend states both gains at every position**, or the knob is dropped. Between two detents
  there is nothing to derive from, and a reader's defaults — silent dry, unity wet — are the reference
  itself, so a middle position that said nothing played as full wet: a hundred per cent wrong at a
  setting the panel offers.
- **`reference` must appear in `positions[]`**, and a knob whose anchor is missing is dropped: without
  that row a player interpolates between neighbours exactly where the knob must be silent, and the
  models already carry that tone, so it lands twice. A **one-position** tone knob is dropped for the
  same family of reasons — its only position is the anchor, which is flat by construction, so the
  control cannot change a sound.
- **`default` on a switch must name a position**, or it is dropped and the knob opens at its reference:
  a pack cannot open on a setting the knob does not have. A dial keeps whatever angle it states.
- **`grid` and `trusted` are for knobs that ship a curve.** A bands-per-position knob carries neither:
  `trusted` is stated in grid indices, and a reader meeting a collapsed band ("tested, a filter
  nowhere") on a knob with no grid reads it as a verdict on bands it should have played.

### Fixed
- The rig conformance harness read `norm`, `sweep` and `operating_point` unconditionally off a const
  JSON — an assert in a debug build and undefined behaviour in a release one. It could not have held a
  switch fixture at all, which is why there had never been one. `rig-measured` now ships one: `edge`,
  a two-position switch shipping bands.

## [3.3.0] — 2026-09-02 — a capture is of one box, and the box has a page

### Added
- **`gear.year` / `gear.serial_number` / `gear.designed_in` / `gear.made_in`** — the PARTICULAR unit a
  stage was captured from, rather than the family its silkscreen names. "Big Muff Pi" has been a dozen
  circuits over fifty years under one name, so make and model alone never identify a device. Where it
  was drawn up and where it was screwed together are two fields and not a sentence, because for gear
  the pair is often the variant itself — a Japanese TS808 and a Taiwanese one are different pedals.
  The year is a JSON integer; the serial is a **string**, because it is an identifier and not a
  quantity. Stamped into every model file as well as the manifest (`gear_year`, `gear_serial_number`,
  `designed_in`, `made_in`), so a lone `.namz` handed to another player still says which unit it came
  off.
- **`url`** (top level) — the PACK's page: where a player can send somebody to hear it or read about
  it. **`gear.url`** — the BOX's page, which is a different link with a different owner: a reader
  handed one of them cannot tell "buy this pedal" from "get this pack". Both are **absolute `https://`
  only, and a reader MUST drop anything else** — a pack is a file from a stranger, and a player that
  follows a `file://` or a `javascript:` out of one has handed that stranger the machine it runs on.
  The reference loader drops such a value without refusing the manifest.
- **`NAMZ_VERSION` is 3.3.0**, and so is the CMake project.

All of the above are optional and additive: `schema` stays 3, absent means the pack did not say
(never "unknown"), and a reader that does not know the keys plays exactly as before.

### Fixed
- **A metadata value with a leading zero stays a string.** The header types `true`/`false` to a bool
  and all-digits to an integer, so a serial number stamped `0012345` went into the file as `12345` and
  came back out of `readMeta` as `"12345"` — a different string than the writer put in, while
  `rig.json`, which types nothing, still said `0012345`: the same box read two ways depending on which
  half of the pack you asked. A leading zero is not a digit of a quantity but part of an identifier,
  and JSON says as much about its own literals (a number may not start with a redundant zero). Bare
  `"0"` is still the number zero. Decided by the value and never by the key — the codec does not know
  which format is stamping it. **Both language ports carry the same rule** (each bumped to 1.1.0), and
  the differential tests hold C++ and JS to byte-identical output on exactly this input.

## [3.2.0] — 2026-08-30 — a pack shows its face

### Added
- **`picture`** — a top-level manifest key naming the device photograph the pack ships (a file in the
  pack root, e.g. `"picture": "device.webp"`): a cut-out with its background removed and alpha kept,
  for players that draw the box they are playing. The capture side scales and encodes it at export;
  a player only draws it. Additive: `schema` stays 3, absent = the pack ships no picture, and a
  reader that does not know the key plays exactly as before.
- **`NAMZ_VERSION` is 3.2.0**, and so is the CMake project.

## [3.1.0] — 2026-08-28 — a file says how late it is

### Added
- **`lag_samples` on a file entry** — how many samples later than the stage's first file this model's
  output arrives, measured once by the capture side with every model in hand. A player that crossfades
  neighbouring captures delays each by the largest lag minus its own, so the pair sums in phase instead
  of combing; until now every player had to push half a second of broadband through every model at
  load to learn a number the packer already knew. Additive: `schema` stays 3, an absent key means
  "not measured", a non-integer is not read, and a reader that does not know the key plays as before.
- `input_db` is documented in NAMZ-FORMAT.md beside it — it had lived only in this changelog.
- **`NAMZ_VERSION` is 3.1.0**, and so is the CMake project.

## [3.0.0] — 2026-08-28 — `tone`, and a knob as bands

### Changed
- **`measured` → `tone`.** The block, the key, and every name in the model (`Measured*` → `Tone*`,
  `Stage::measured` → `Stage::tone`). `measured` is not read: no pack carrying it was ever published.
- **`schema` 2 → 3.** Had the number stayed, a schema-2 reader would have found no `measured` key and
  played every tone knob flat, silently; at 3 it refuses instead. A `schema` that is not a JSON integer
  is refused too — the gate used to read `4.0` and `"4"` as 1, the oldest vintage there is.
- **One form per knob.** A `tone` knob carries `positions` OR `sections`, never both; both on one knob
  is an invalid manifest, refused whole (`ok = false`, empty chain — the same severity as a schema this
  reader does not speak, and through `loadRig` the same headers-only fallback). `positions` stays in
  the format permanently.
- **`NAMZ_VERSION` is 3.0.0**, and so is the CMake project — moved with the tag this time.
- **`rig.json` is laid out for a reader with eyes.** Keys in the order the format lists them (it was
  the alphabet's: `chain` before `format`, a band's `type` last); a control, a file entry, a band, a
  position, `gear`, `grid`, `trusted` each on one line; a curve's points and a dial's values inline,
  wrapped under their first element; `"format", "schema"` and the identity on the two lines that open
  the manifest. Not one value changed — a reader parses JSON and sees the same document; only the
  bytes of the golden packs moved, regenerated by the same writer.

### Added
- **`sections`** — the second form of a tone knob: parametric RBJ bands (`type` `low_shelf` | `bell` |
  `high_shelf` | `tilt`, `hz`, `q`, a SIGNED `range_db` at the two stops; optional `pivot` on a tilt,
  `hz_at` / `q_at` at the stops). The band formulas, the tilt trim, and the travel law — gain zero at
  `reference` and linear in dB to each stop; frequency geometric, Q linear, the stop chosen by the sign
  of `t`; `t` from POSITION, never from gain — are normative in NAMZ-FORMAT.md and identical to the
  capture side's `ToneSections.h`.
- `conformance/rig-measured` carries a second knob, `bass`, shipped as `sections` beside the `tone`
  ladder: a low shelf that travels, and a tilt with a fitted hinge.
- Tests for what was never tested: the schema gate (own, one above, older, float, string), both forms
  on one knob, every way a band can be unreadable, and the sections round trip.

### Fixed
- A `positions` knob loads only what it can play: a position without `db`, or with a `db` that is not
  exactly `grid.points` numbers, is skipped, and a knob with no grid or no position left is dropped —
  what NAMZ-FORMAT.md has said since 2.0.0 and what the loader did not do. A reader downstream no
  longer discovers a short curve by indexing past it.
- The `rig-measured` golden pack carried `trusted.lo_index` / `hi_index` of 0 / 0 and `level_db` of
  0.0 on every position while `expected.json` stated 1 / 6 and real levels: the regeneration recipe
  never copied them and the check never compared them. Both do now, and the pack says what the fixture
  says.

## [2.0.0] — 2026-08-07 — the `.orbitrig` rig layer

The codec described below packs one model. This layer describes a DEVICE: `rig.json` plus the `.namz`
files it names, so a player can pick a model by a knob and apply the knobs that are not models.

An earlier draft of it went out under **v1.1.0 / v1.1.1** without a changelog entry — those tags carry
the rig layer with a fitted 1-pole where the curve now is, and `schema` still reading 1. They are
superseded, and this entry describes the layer as it actually stands. The codec itself
(`.namz`, wire format 2) is untouched by all of it.

### Added
- **`namz_rig.h` / `_load.h` / `_write.h`** — the manifest: a `chain` of stages, each with its `gear`,
  its captured `controls` and the files that realise them.
- **`measured`** — a linear control shipped as a dB magnitude table per position, relative to a stated
  `reference` the models were all captured at. The player interpolates and builds a filter; the knob it
  draws is continuous.
- **`trusted`** — the band where that table was shown to BE a filter, over a stated drive range, with
  the number of levels that showed it. Also as grid indices, so two readers cannot disagree about which
  point an edge meant.
- **`blend`** — a dry/wet mix: the dry path's own response and level, per-position gains, a polarity
  defined against the model's output, and whether those gains were measured or assumed. Costs no extra
  captures, because the dry signal is the DI the model is already fed.
- **Per-value `labels`**, a per-position `level_db`, and a `default` position — three things a player
  cannot derive and would otherwise invent.
- **Conformance** — `conformance/rig/` pins the selection policy on a sparse matrix;
  `conformance/rig-measured/` pins the shape a client is written against.

### Removed
- **`lp1_hz` / `gain_db` / `residual_db`** — a fitted 1-pole beside the curve, so a simple player could
  skip the table. Measured on real hardware the best fit missed a Big Muff tone control by 15 to 65 dB.
  Removed rather than deprecated: everything parsed, nothing complained, and the result was confidently
  wrong.
- **The clock vocabulary** (`12h`, `07h`) — degrees only. A knob's position is a number, and a filename
  means nothing.

### Changed
- **`schema` 1 → 2.** None of the above is additive: a reader built for schema 1 would parse such a
  manifest, recognise the keys it knows, drop the rest and be confidently wrong about every measured
  knob. That is the failure the number exists to prevent, and it stayed at 1 through all of it. The
  writer now spells it from the constant, so the two ends cannot drift apart again.
- **`input_db` on a file entry** — one setting playing another's model with less signal going *into*
  it, which is how the bottom of a gain dial fades instead of becoming a model of hiss.
- **`trusted` ships only when levels were actually compared** — an untested claim now says so by being
  absent, instead of by carrying a default that reads like a measurement.

### Fixed
- **`NAMZ_VERSION` told the truth for the first time since 1.0.0.** The macros in `namz.h` read
  `"1.0.0"` while the tags walked to v1.1.1; a header that misreports itself is worse than one that
  says nothing.

### Known consequence
A schema-1 pack that carried a `measured` control reads back with that knob silent — its old shape is
no longer parsed. No such pack was ever published, so this costs nothing today.

## [1.0.0] — 2026-07-05

First stable release of the C++ reference.

### Added
- **`include/namz.h`** — single-header (stb-style) lossless `.nam` ↔ `.namz` codec: `pack` / `unpack` /
  `readMeta` / `isNamz`. JUCE-free — C++17 std + nlohmann/json. Stores NAM weights as float32 instead of
  ~20-char decimal text (~5.5× smaller, bit-exact to the engine's float32), byte-plane shuffle, and a
  cheap readable metadata header.
- **CLI** (`cli/namz.cpp`) — `encode` / `decode` / `map` / `verify`.
- **Wire-format spec** — [`NAMZ-FORMAT.md`](NAMZ-FORMAT.md), with reserved codec/dtype bytes for growth.
- **Tests** — `tests/roundtrip.cpp` and the maniacal, cross-platform-hostile `tests/adversarial.cpp`
  (golden bytes, little-endian invariants, a no-trailing-slack guard, 100k-float32 fuzz, bit-flip fuzz,
  hand-crafted malicious containers, v1 back-compat, deep nesting, a unicode zoo, a comma-decimal locale,
  oversized metadata).
- **Conformance vectors** (`conformance/`) — the cross-language TCK: golden input→output pairs +
  must-reject invalid blobs + a manifest, consumed by `tests/conformance.cpp`.
- **CI** — build + test on Linux, macOS, and Windows.

### Guarantees
- **Lossless** to float32, **deterministic** (byte-identical across runs, platforms, and — via the
  conformance vectors — language ports), and **robust** (every malformed input is rejected cleanly).

[3.0.0]: https://github.com/darwinscat/namz/releases/tag/v3.0.0
[2.0.0]: https://github.com/darwinscat/namz/releases/tag/v2.0.0
[1.0.0]: https://github.com/darwinscat/namz/releases/tag/v1.0.0
