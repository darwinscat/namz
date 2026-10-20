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
npm install @darwins-cat/namz
```

Node ≥ 18. Works in browsers/bundlers too (uses only `TextEncoder`/`DataView`/`Float32Array`).

## Library

```js
import { pack, unpack, readMeta, isNamz } from "@darwins-cat/namz";

const nam = new TextEncoder().encode('{"architecture":"WaveNet","weights":[0.5,-0.5,0.25]}');

const blob = pack(nam);               // -> Uint8Array (.namz)
isNamz(blob);                         // true
new TextDecoder().decode(unpack(blob)); // '{"architecture":"WaveNet","weights":[0.5,-0.5,0.25]}'

// Stamp a readable metadata header (typed: "true"/"false" -> bool, digits -> int, else string;
// a leading zero keeps it a string, so an identifier such as "0012345" survives intact):
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

`options`: `{ shuffle = true, metadata = {}, dtype = DTYPE_F32 }`. `unpack` is total: for **any** input it returns a
`Uint8Array` (empty on failure) and never throws.

### Float16 — `.nam16z` (lossy, opt-in)

```js
import { pack, DTYPE_F16 } from "@darwins-cat/namz";

const half = pack(nam, { dtype: DTYPE_F16 }); // each weight's float32 -> the nearest IEEE half (ties to even)
```

**Float16 is lossy, and it is written only when asked for.** Each weight is rounded to the IEEE half
nearest its float32 — up to half a step of an 11-bit significand — for a payload half the size; float32
(`DTYPE_F32`) stays the default and the only lossless dtype. `pack` refuses the whole `.nam` (empty result)
if a weight would pass half's range (|w| ≥ 65520; up to there it rounds to 65504 at most). Name such a file
`.nam16z`, so the loss shows in its name. `unpack` and `readMeta` need no option: they go by the dtype
byte in the file, never its name, and widen each half to the float32 of the same value, exactly. The bytes
are the C++ reference's (4.4.0) for the same input, checked against the shared vectors.

## CLI

```bash
namz encode in.nam out.namz [--no-shuffle] [--set key=value ...]
namz encode in.nam out.nam16z --f16 [--no-shuffle] [--set key=value ...]   # float16, LOSSY
namz decode in.namz|in.nam16z out.nam
namz map    in.namz|in.nam16z [--json]   # print the metadata header (no weight decode)
namz verify in.nam [--f16]          # pack->unpack round-trip check + ratio
```

`--f16` writes only a `*.nam16z`, and a `*.nam16z` only with `--f16`; either mismatch is refused (exit 2)
before anything is read.

## Guarantees

- **Lossless to float32** — `unpack(pack(x))` is bit-exact at any nesting depth, including `-0.0`,
  subnormals, and `FLT_MAX`. That is the default dtype's promise; float16 is lossy by design and opt-in.
- **Deterministic** — `pack(x)` is byte-identical across runs and platforms; the codec is idempotent.
- **Robust** — every malformed input (bad magic, unknown version/codec/dtype, truncation, trailing junk,
  a lying `metaLen`/`numArrays`, a placeholder naming no segment, an over-cap body) is rejected cleanly.
  Any JSON nested deeper than `MAX_DEPTH` (128) is refused — a `.nam` by pack, a skeleton by unpack, a
  metadata block reads as none — without the parser ever following it down; and pack refuses a `.nam` whose
  `"weights"` member holds an integer literal, which would read back as a placeholder.

### Byte-match note

JavaScript has a single `Number` type, so this port ships its own JSON parse/serialize that (a) preserves
the integer-vs-float distinction the reference relies on (so `-18.0` stays `-18.0`, not `-18`), (b) sorts
object keys by UTF-8 byte order like nlohmann's `std::map`, and (c) prints every double with nlohmann's own
digits: a port of its Grisu2 (`dtoa_impl`), which is neither always the shortest nor always the closest
(`-198.70986938476562` prints as `-198.70986938476563`), and of its switch between fixed and exponent form
(`1e+15`, `1e-05`). Integer literals are kept exact via `BigInt` across the full int64/uint64 range. The
parser refuses what nlohmann refuses — a number past every double, an escaped lone surrogate — wherever it
stands, a value a later duplicate key replaces included. This is validated byte-for-byte against the
reference: the shared vectors, and a differential run over random doubles and float32 values when a built
reference CLI is around (`NAMZ_CLI`).

- A weight whose float32 is not finite (`1e39`) is refused by pack, and a payload float that is NaN or
  infinite by unpack, exactly as the reference refuses them.

## Development

```bash
npm test        # runs the shared conformance vectors + property fuzz + adversarial suite
```

Versioned independently of the C++ reference; tracks **wire format version 2**.

## License

MIT — see [LICENSE](LICENSE). © 2026 Oleh Tsymaienko &lt;oleh@darwinscat.com&gt; &amp; Alisa Lafoks
&lt;alisa@darwinscat.com&gt; (Darwin's Cat).
