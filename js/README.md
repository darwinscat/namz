<!-- SPDX-License-Identifier: MIT -->
# namz (JavaScript)

Lossless codec for [NeuralAmpModeler](https://github.com/sdatkinson/neural-amp-modeler) `.nam` files.

A `.nam` is JSON whose bulk is one or more flat `"weights"` arrays written as ~20-character decimal
strings — which the NAM engine loads into `float32` **anyway**. `.namz` stores each weight as a 4-byte
float32 instead: **≈5.5× smaller**, **bit-exact** to what the engine computes, and **deterministic**
(no compression → byte-identical across runs, platforms, and languages). A small readable header exposes
tone/gear/device metadata without decoding the weights.

This package is a **native JavaScript port** (zero dependencies, pure ESM) of the C++ reference and is
**byte-for-byte compatible** with it and with every other port — validated against the shared
[conformance vectors](https://github.com/darwinscat/namz/tree/main/conformance) (the cross-language TCK)
plus a live differential fuzz against the reference binary.

## Install

```bash
npm install namz
```

Node ≥ 18. Works in browsers/bundlers too (uses only `TextEncoder`/`DataView`/`Float32Array`).

## Library

```js
import { pack, unpack, readMeta, isNamz } from "namz";

const nam = new TextEncoder().encode('{"architecture":"WaveNet","weights":[0.5,-0.5,0.25]}');

const blob = pack(nam);               // -> Uint8Array (.namz)
isNamz(blob);                         // true
new TextDecoder().decode(unpack(blob)); // '{"architecture":"WaveNet","weights":[0.5,-0.5,0.25]}'

// Stamp a readable metadata header (typed: "true"/"false" -> bool, digits -> int, else string):
const withMeta = pack(nam, { metadata: { tone_type: "hi-gain", boost: "true", device: "tube:1,pnp:1" } });
readMeta(withMeta); // { tone_type: 'hi-gain', boost: 'true', device: 'tube:1,pnp:1' } — no weight decode
```

### API

| function | description |
|---|---|
| `pack(namJson, options?) -> Uint8Array` | `.nam` (Uint8Array/ArrayBuffer/string) → `.namz`. Empty on invalid JSON. |
| `unpack(data, maxJsonBytes?) -> Uint8Array` | `.namz` → `.nam` JSON. Empty on corruption / over-cap. Never throws. |
| `readMeta(data) -> object` | display header without touching weights. `{}` for v1 / non-`.namz`. |
| `isNamz(data) -> boolean` | cheap magic check. |

`options`: `{ shuffle = true, metadata = {} }`. `unpack` is total: for **any** input it returns a
`Uint8Array` (empty on failure) and never throws.

## CLI

```bash
namz encode in.nam out.namz [--no-shuffle] [--set key=value ...]
namz decode in.namz out.nam
namz map    in.namz [--json]        # print the metadata header (no weight decode)
namz verify in.nam                  # pack->unpack round-trip check + ratio
```

## Guarantees

- **Lossless to float32** — `unpack(pack(x))` is bit-exact at any nesting depth, including `-0.0`,
  subnormals, and `FLT_MAX`.
- **Deterministic** — `pack(x)` is byte-identical across runs and platforms; the codec is idempotent.
- **Robust** — every malformed input (bad magic, unknown version/codec/dtype, truncation, trailing junk,
  a lying `metaLen`/`numArrays`, an over-cap body) is rejected cleanly.

### Byte-match note

JavaScript has a single `Number` type, so this port ships its own JSON parse/serialize that (a) preserves
the integer-vs-float distinction the reference relies on (so `-18.0` stays `-18.0`, not `-18`), (b) sorts
object keys by UTF-8 byte order like nlohmann's `std::map`, and (c) reproduces nlohmann's exact
double-to-string formatting (fixed vs scientific thresholds, `e±XX` exponents). Integer literals are kept
exact via `BigInt` across the full int64/uint64 range. This is validated byte-for-byte against the
reference over the whole NAM domain. Two documented edges, both outside that domain:

- Non-finite weights (`NaN`/`±Inf`) are out of contract and are **rejected**, exactly as the reference
  rejects them.
- A *non-weight* number whose magnitude exceeds ~1e21 (or an integer past `uint64`) may differ by one ULP
  — not from formatting, but because nlohmann's own number *parser* is not correctly rounded there. Such
  values never occur in NAM config.

## Development

```bash
npm test        # runs the shared conformance vectors + property fuzz + adversarial suite
```

Versioned independently of the C++ reference; tracks **wire format version 2**.

## License

MIT — see [LICENSE](LICENSE). © 2026 Oleh Tsymaienko &lt;oleh@darwinscat.com&gt; &amp; Alisa Lafoks
&lt;alisa@darwinscat.com&gt; (Darwin's Cat).
