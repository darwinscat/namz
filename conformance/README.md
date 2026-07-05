<!-- SPDX-License-Identifier: MIT -->
# Conformance vectors — the cross-language TCK

These files are the **contract every language implementation of namz must satisfy.** They are how a
Python / JS / Rust / … port proves it is byte-for-byte compatible with the C++ reference — no port is
"the" binary; the vectors are the source of truth.

```
manifest.json         machine-readable description of every case
vectors/<name>.nam    a valid input
vectors/<name>.namz   the reference output — your encode() must produce these exact bytes
invalid/<name>.namz   malformed — your decode() must reject it (error/empty), never crash
generate.py           regenerates everything from the reference CLI (see below)
```

## What a port must check

For every entry in `manifest.json["valid"]`:
1. **encode** the `input` with the given `shuffle` flag and `set` metadata → must **byte-match** `output`.
2. **decode** the `output` → the weights must be **bit-exact** float32.
3. decode→encode must reproduce `output` (idempotence).

For every entry in `manifest.json["invalid"]`:
- **decode** the file → must be **rejected** (empty/error) and must not crash or hang.

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
