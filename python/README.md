<!-- SPDX-License-Identifier: MIT -->
# namz (Python)

Lossless codec for [NeuralAmpModeler](https://github.com/sdatkinson/neural-amp-modeler) `.nam` files.

A `.nam` is JSON whose bulk is one or more flat `"weights"` arrays written as ~20-character decimal
strings — which the NAM engine loads into `float32` **anyway**. `.namz` stores each weight as a 4-byte
float32 instead: **≈5.5× smaller**, **bit-exact** to what the engine computes, and **deterministic**
(no compression → byte-identical across runs, platforms, and languages). A small readable header exposes
tone/gear/device metadata without decoding the weights.

This package is a **native Python port** of the C++ reference and is **byte-for-byte compatible** with it
and with every other port — it is validated against the shared [conformance vectors](https://github.com/darwinscat/namz/tree/main/conformance)
(the cross-language TCK).

## Install

```bash
pip install namz
```

Requires Python ≥ 3.9 and NumPy.

## Library

```python
import namz

nam = b'{"architecture":"WaveNet","weights":[0.5,-0.5,0.25]}'

blob = namz.pack(nam)                 # -> .namz bytes
assert namz.is_namz(blob)
assert namz.unpack(blob) == nam       # lossless to float32

# Stamp a readable metadata header (typed: "true"/"false" -> bool, digits -> int, else string):
opts = namz.PackOptions(metadata={"tone_type": "hi-gain", "boost": "true", "device": "tube:1,pnp:1"})
blob = namz.pack(nam, opts)
namz.read_meta(blob)   # {'tone_type': 'hi-gain', 'boost': 'true', 'device': 'tube:1,pnp:1'} — no weight decode
```

### API

| function | description |
|---|---|
| `pack(nam_json, options=None) -> bytes` | `.nam` JSON (str/bytes) → `.namz`. `b""` on invalid JSON. |
| `unpack(data, max_json_bytes=256MiB) -> bytes` | `.namz` → `.nam` JSON. `b""` on corruption / over-cap. Never raises. |
| `read_meta(data) -> dict[str, str]` | display header without touching weights. `{}` for v1 / non-`.namz`. |
| `is_namz(data) -> bool` | cheap magic check. |
| `PackOptions(shuffle=True, metadata={})` | packing options. |

`unpack` is total: for **any** input bytes it returns `bytes` (empty on failure) and never raises.

## CLI

```bash
namz encode in.nam out.namz [--no-shuffle] [--set key=value ...]
namz decode in.namz out.nam
namz map    in.namz [--json]        # print the metadata header (no weight decode)
namz verify in.nam                  # pack->unpack round-trip check + ratio
```

(Also available as `python -m namz`.)

## Guarantees

- **Lossless to float32** — `unpack(pack(x))` is bit-exact at any nesting depth, including `-0.0`,
  subnormals, and `FLT_MAX`.
- **Deterministic** — `pack(x)` is byte-identical across runs and platforms; the codec is idempotent.
- **Robust** — every malformed input (bad magic, unknown version/codec/dtype, truncation, trailing junk,
  a lying `metaLen`/`numArrays`, an over-cap body) is rejected cleanly.

### Byte-match note

The skeleton is minified JSON with keys sorted ascending, matching the reference (nlohmann). It is
validated byte-for-byte against the reference over the whole NAM domain (a large differential fuzz —
config integers, simple decimals, non-ASCII, control chars, nesting). The weight payload itself is always
exact float32, never text. Two documented edges, both outside the NAM domain:

- Non-finite weights (`NaN`/`±Inf`) are out of contract and are **rejected**, exactly as the reference
  rejects them.
- An integer literal outside `[int64_min, uint64_max]` in a *non-weight* field is coerced to a double, as
  nlohmann does; this is byte-exact for double-representable magnitudes (e.g. `2**64`). For extreme
  23+-digit integers nlohmann's own parser rounds imprecisely and may differ by one ULP — such values
  never occur in NAM config.

## Development

```bash
pip install -e ".[test]"
pytest                      # runs the shared conformance vectors + property fuzz + adversarial suite
```

Versioned independently of the C++ reference; tracks **wire format version 2**.

## License

MIT — see [LICENSE](LICENSE). © 2026 Oleh Tsymaienko &lt;oleh@darwinscat.com&gt; &amp; Alisa Lafoks
&lt;alisa@darwinscat.com&gt; (Darwin's Cat).
