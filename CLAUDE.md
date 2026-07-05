# namz — repo guide for Claude

**namz** is a lossless codec for NeuralAmpModeler `.nam` files: a single C++17 header (`include/namz.h`) +
a CLI (`cli/namz.cpp`) + a wire-format spec (`NAMZ-FORMAT.md`). It stores NAM weights as float32 instead of
decimal text (~5.5× smaller, bit-exact, deterministic). Extracted from the OrbitCab plugin; its output is
byte-identical to OrbitCab's embedded `.namz`.

## 👉 If your task is porting namz to other languages
**Read [`docs/LANGUAGE-PORTS-BRIEF.md`](docs/LANGUAGE-PORTS-BRIEF.md) first** — it is the full mission brief
(what to build, the testing model via `conformance/`, language priority, packaging, constraints). Start
with Python, end-to-end, then review before the next language.

## Non-negotiables (apply to ALL work in this repo)
- **Respond to the user (Oleh) in Russian.** He's a senior Java/enterprise engineer, new to native +
  multi-language publishing — pitch there, use JVM analogies.
- **Authorship:** every commit and PR is `Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks
  <alisa@darwinscat.com>`. **Never** add an AI co-author or "generated with" trailer. Everything is from Oleh.
- **License: MIT** (matches NAM, which is MIT). New deps must be permissive.
- **Byte-identical is the law.** Any change must keep the `conformance/` vectors passing; the C++ reference
  (`include/namz.h`) + the CLI are the source of truth. Regenerate vectors only via
  `conformance/generate.py`, never hand-edit a golden to fit code.
- **Determinism:** no compression, integer fields little-endian, minified + key-sorted JSON skeleton.

## Layout
- `include/namz.h` — the codec (header-only, stb-style; define `NAMZ_IMPLEMENTATION` in one TU).
- `cli/namz.cpp` — `encode` / `decode` / `map` / `verify`.
- `NAMZ-FORMAT.md` — the wire format (has reserved codec/dtype bytes for growth).
- `conformance/` — the cross-language TCK (golden pairs + invalid blobs + `manifest.json`).
- `tests/` — `roundtrip.cpp`, the maniacal `adversarial.cpp`, and `conformance.cpp` (the vector runner).
- `.github/workflows/ci.yml` — build + test on Linux / macOS / Windows.

## Build & test
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```
