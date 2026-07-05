<!-- SPDX-License-Identifier: MIT -->
# Changelog

All notable changes to **namz** are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project uses
[Semantic Versioning](https://semver.org/). The C++ reference versions independently; each language port
carries its own version.

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

[1.0.0]: https://github.com/darwinscat/namz/releases/tag/v1.0.0
