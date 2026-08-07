<!-- SPDX-License-Identifier: MIT -->
# Changelog

All notable changes to **namz** are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project uses
[Semantic Versioning](https://semver.org/). The C++ reference versions independently; each language port
carries its own version.

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

[2.0.0]: https://github.com/darwinscat/namz/releases/tag/v2.0.0
[1.0.0]: https://github.com/darwinscat/namz/releases/tag/v1.0.0
