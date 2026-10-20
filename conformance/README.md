<!-- SPDX-License-Identifier: MIT -->
# Conformance vectors — the cross-language TCK

These files are the **contract every language implementation of namz must satisfy.** They are how a
Python / JS / Rust / … port proves it is byte-for-byte compatible with the C++ reference — no port is
"the" binary; the vectors are the source of truth.

```
manifest.json         machine-readable description of every case
vectors/<name>.nam    a valid input
vectors/<name>.namz   the reference output — your encode() must produce these exact bytes
vectors/<name>.nam16z the same, for a case packed as float16 ("dtype": 1)
invalid/<name>.namz   malformed — your decode() must reject it (error/empty), never crash
decode-only/<name>.namz + <name>.nam
                      a .namz pack never writes — your decode() must produce that .nam byte for byte
invalid/<name>.nam    a .nam your encode() must refuse (error/empty)
generate.py           regenerates everything from the reference CLI (see below)
```

## What a port must check

For every entry in `manifest.json["valid"]`:
1. **encode** the `input` with the given `shuffle` flag, `set` metadata and `dtype` (0 float32, 1 float16 —
   the reference's `PackOptions::dtype`) → must **byte-match** `output`.
2. **decode** the `output` → the weights must be **bit-exact** float32; for dtype 1, each the half nearest
   the input's float32, ties to even, widened (NAMZ-FORMAT.md "The float16 dtype").
3. decode→encode must reproduce `output` (idempotence).

For every entry in `manifest.json["invalid"]`:
- **decode** the file → must be **rejected** (empty/error) and must not crash or hang.

For every entry in `manifest.json["decodeOnly"]`:
- **decode** `file` → must **byte-match** `output`. These are containers `pack` never writes — a
  placeholder spelled `-0`, a `weights` that is `1.0` or `0e0` or past uint64, two `weights` keys, a
  version-1 file, float32 values whose digits the reference picks its own way — so they pin what a reader
  takes from the literal as written and how it prints a number (see NAMZ-FORMAT.md "Placeholders and
  depth"): a language whose JSON parser forgets `1.0` from `1`, rounds integers past 2^53, or prints its own
  shortest digits instead of the reference's Grisu2 ones, fails here.

For every entry in `manifest.json["invalidNam"]`:
- **encode** the file with its `dtype` → must be **refused** (empty/error): a `.nam` holding an integer
  `weights` member, one nested deeper than 128 (as written, a member a later duplicate key replaces
  included), one in UTF-16, one escaping a lone surrogate, one holding a weight past the float32 range — and,
  with dtype 1, one holding a weight past half's range (|w| ≥ 65520), which packs fine as float32.

The float16 cases (`f16-*`, `bad-dtype-2`) pin dtype 1 end to end: goldens in both plane layouts (a nested
`SlimmableContainer` with metadata, subnormal halves and their ties, ±0, 65504 and the edge below 65520,
ties to even, an empty array), decode-only halves widened and printed as the reference prints them (across
two segments, and in a version-1 file), and the refusals — a payload that is no whole number of halves or
not `sum(lengths)` of them, an infinite or NaN half, a float16 file relabelled float32, dtype 2.

Plus each port should add its own **local property fuzz** (generate N random finite float32, round-trip,
assert bit-exact) — that part isn't shared bytes, it's a per-language property.

## Byte-match gotchas (why an encode might differ)

- The skeleton is **minified JSON with object keys sorted ascending** (the reference uses nlohmann, which
  sorts). Match it — Python: `json.dumps(obj, sort_keys=True, separators=(",", ":"))`; JS: `JSON.stringify`
  does **not** sort, so you need a sorted+compact stringify.
- Integers are **little-endian**; the float payload is **IEEE-754 little-endian** (numpy:
  `arr.astype("<f4").tobytes()`).
- The byte-plane **shuffle**: split each float's 4 bytes into 4 contiguous planes on encode; reverse on
  decode (see `shuffleInto`/`unshuffleInto` in `include/namz.h`).

If a language genuinely cannot match key order, rely on **decode-bit-exact + idempotence** and document it
— never edit a golden file to fit a buggy port.

## Regenerating

```bash
cmake --build build            # builds the reference CLI at build/namz
python3 conformance/generate.py build/namz
```

The reference C++ consumer is `tests/conformance.cpp` (run via `ctest -R namz_conformance`).
