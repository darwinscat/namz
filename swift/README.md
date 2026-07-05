<!-- SPDX-License-Identifier: MIT -->
# namz (Swift)

Lossless codec for [NeuralAmpModeler](https://github.com/sdatkinson/neural-amp-modeler) `.nam` files —
for iOS / iPadOS / macOS AUv3 guitar apps.

A `.nam` is JSON whose bulk is one or more flat `"weights"` arrays written as ~20-character decimal
strings — which the NAM engine loads into `float32` **anyway**. `.namz` stores each weight as a 4-byte
float32 instead: **≈5.5× smaller**, **bit-exact** to what the engine computes, and **deterministic**
(no compression → byte-identical across runs, platforms, and languages). A small readable header exposes
tone/gear/device metadata without decoding the weights.

This package is a **native Swift port** of the C++ reference and is **byte-for-byte compatible** with it
and with every other port — validated against the shared
[conformance vectors](https://github.com/darwinscat/namz/tree/main/conformance) (the cross-language TCK)
plus a live differential fuzz against the reference binary.

## Install (SwiftPM)

```swift
.package(url: "https://github.com/darwinscat/namz.git", from: "1.0.0")
```

then add `"Namz"` to your target's dependencies. Depends only on the standard library + Foundation.

## Library

```swift
import Namz

let nam = Array("{\"architecture\":\"WaveNet\",\"weights\":[0.5,-0.5,0.25]}".utf8)

let blob = Namz.pack(nam)                  // [UInt8] (.namz)
Namz.isNamz(blob)                          // true
String(decoding: Namz.unpack(blob), as: UTF8.self)  // the original JSON, weights bit-exact

// Stamp a readable metadata header (typed: "true"/"false" -> bool, digits -> int, else string):
let opts = PackOptions(metadata: ["tone_type": "hi-gain", "boost": "true", "device": "tube:1,pnp:1"])
Namz.readMeta(Namz.pack(nam, options: opts))  // ["tone_type": "hi-gain", ...] — no weight decode
```

`Data` overloads are provided for all four entry points (`pack`/`unpack`/`readMeta`/`isNamz`).

### API

| function | description |
|---|---|
| `Namz.pack(_:options:) -> [UInt8]` | `.nam` bytes → `.namz`. Empty on invalid JSON / non-finite weight. |
| `Namz.unpack(_:maxJSONBytes:) -> [UInt8]` | `.namz` → `.nam` JSON. Empty on corruption / over-cap. Never traps. |
| `Namz.readMeta(_:) -> [String: String]` | display header without touching weights. `[:]` for v1 / non-`.namz`. |
| `Namz.isNamz(_:) -> Bool` | cheap magic check. |

## CLI

```bash
swift run namz encode in.nam out.namz [--no-shuffle] [--set key=value ...]
swift run namz decode in.namz out.nam
swift run namz map    in.namz [--json]
swift run namz verify in.nam
```

## Guarantees

- **Lossless to float32** — `unpack(pack(x))` is bit-exact at any nesting depth, including `-0.0`,
  subnormals, and `FLT_MAX`.
- **Deterministic** — `pack(x)` is byte-identical across runs and platforms; the codec is idempotent.
- **Robust** — every malformed input (bad magic, unknown version/codec/dtype, truncation, trailing junk,
  a lying `metaLen`/`numArrays`, an over-cap body) is rejected cleanly.

### Byte-match note

Foundation's JSON APIs can't reproduce the reference bytes, so this port carries its own JSON model that
(a) preserves the integer-vs-float distinction the reference relies on (`-18.0` stays `-18.0`), (b) sorts
object keys by UTF-8 byte order like nlohmann's `std::map`, (c) keeps integers exact across the full
int64/uint64 range (coercing beyond to double, as nlohmann does), and (d) reproduces nlohmann's exact
double-to-string formatting (fixed vs scientific thresholds, `e±XX` exponents), verified against the
reference binary. Two documented edges, both outside the NAM domain: non-finite weights (`NaN`/`±Inf`)
are rejected as the reference rejects them; a non-weight number beyond ~1e21 (or an integer past uint64)
may differ by one ULP because nlohmann's own parser is not correctly rounded there — such values never
occur in NAM config.

## Development

```bash
swift build
swift run namz-conformance   # runs the shared vectors + property fuzz + adversarial + differential
```

The test runner is a portable executable (no XCTest dependency), so it runs on a CommandLineTools-only
macOS too. Set `NAMZ_CLI=/path/to/build/namz` to enable the live differential check against the C++
reference. Versioned independently of the C++ reference; tracks **wire format version 2**.

## License

MIT — see [LICENSE](LICENSE). © 2026 Oleh Tsymaienko &lt;oleh@darwinscat.com&gt; &amp; Alisa Lafoks
&lt;alisa@darwinscat.com&gt; (Darwin's Cat).
