<!-- SPDX-License-Identifier: MIT -->
# namz

**A tiny, lossless codec for [NeuralAmpModeler](https://github.com/sdatkinson/NeuralAmpModelerCore) `.nam` files.** Single C++17 header + a small CLI.

A `.nam` is JSON whose bulk is one or more flat `"weights"` arrays written as full-precision **decimal
strings** (~20 chars per number). The NAM engine loads those weights into `std::vector<float>` — so the
decimals are truncated to **float32 on load anyway**.

`.namz` stores each weight as a 4-byte **float32** instead of ~20 bytes of text: **≈5.5× smaller** than
the raw JSON, and **bit-exact** to what the engine computes (zero quality loss). Everything else
(architecture / config / metadata) is preserved verbatim.

- **Lossless** to the float32 the engine actually uses — verified across `-0.0`, subnormals, `FLT_MAX`.
- **Deterministic** — the output is byte-identical run-to-run and across platforms (no compressor, no
  timestamps), so packed files are safe to commit to git.
- **Dependency-light** — one header, C++17 std + [nlohmann/json](https://github.com/nlohmann/json). No zlib.
- **Fast to load** — no decompression. `unpack` un-shuffles the bytes and re-emits the `.nam` JSON for
  anything that takes a `.nam`; `decode` skips the text altogether and hands a loader the parsed skeleton
  and the weights as `std::vector<float>` — on a real 14,017-float capture **0.03 ms against 2.3 ms** for
  `unpack` + `json::parse` (native, Release).
- **Cheap metadata** — a small header block (tone / device / gear / …) is readable **without** decoding
  the weights.
- **Float16 on request** — `.nam16z`: the same container with each weight rounded to an IEEE half, for when
  bytes matter more than the last bits (a web download). **Lossy**, so never the default — see below.

The float bytes are byte-plane **shuffled** but not otherwise compressed (`codec = store`). The shuffle is
free and helps whatever **outer** compressor sees the file (an installer's LZMA, git's packing); an inner
deflate there is redundant and would only cost determinism and a zlib dependency.

## Use the header

Single-header, stb-style. In exactly **one** translation unit define the implementation:

```cpp
#define NAMZ_IMPLEMENTATION
#include "namz.h"          // needs <nlohmann/json.hpp> on the include path
```

```cpp
std::vector<uint8_t> nam = /* raw .nam JSON bytes */;

// pack (optionally stamp display metadata)
namz::PackOptions opts;
opts.metadata["tone_type"] = "hi-gain";
opts.metadata["device"]    = "tube:1,pnp:1";
std::vector<uint8_t> packed = namz::pack(nam.data(), nam.size(), opts);

// read the metadata WITHOUT decoding weights
std::map<std::string,std::string> meta = namz::readMeta(packed.data(), packed.size());

// unpack back to .nam JSON (bit-exact float32); the size arg is a zip-bomb cap
std::vector<uint8_t> back = namz::unpack(packed.data(), packed.size(), 256u*1024u*1024u);
```

### Float16 — `.nam16z` (lossy, on request)

```cpp
namz::PackOptions opts;
opts.dtype = namz::DtypeF16;           // each weight's float32 -> the nearest IEEE half (ties to even)
std::vector<uint8_t> half = namz::pack(nam.data(), nam.size(), opts);   // empty: a |w| >= 65520 (rounds past 65504)
```

The payload halves: on the real captures a file is **0.55×** its float32 `.namz`, and **0.52×** after
`brotli -q 11` (an IR-X pack of 54 captures: 2.64 MB → 1.37 MB). It is **lossy** — each weight moves by up
to half a step of an 11-bit significand — so `pack` writes it only when `dtype` asks, and the file is
conventionally named `.nam16z` so the loss shows in its name. The readers need no option: `unpack`,
`decode` and `readMeta` go by the dtype byte in the file, never by its name, and widen each half to the
float32 of the same value, exactly. The skeleton and the metadata are the same bytes as the float32 file's.
The js and python ports read and write it too, byte-identical, from their 1.3.0 (`{ dtype: DTYPE_F16 }` /
`PackOptions(dtype=DTYPE_F16)`, and `--f16` in their CLIs), and the conformance vectors pin it.

`pack`/`unpack`/`readMeta` are byte-in / byte-out — no JUCE, no framework — so the same header drops into
a plugin, a tool, or a script.

### Load a model without printing its weights

`unpack` writes every weight back out as decimal text, and a NAM loader then parses that text straight
back into the floats the file already held. A loader that builds its model itself can skip the round
trip: `decode` hands it the skeleton already parsed — the `.nam` with each `"weights"` array replaced by
the integer index of its segment — and the weights as `std::vector<float>`, the type NAM's engine takes.

```cpp
// the size arg is a zip-bomb cap in floats: what the file stores, and what its placeholders name
std::optional<namz::Decoded> d = namz::decode(packed.data(), packed.size(), 16u*1024u*1024u);
if (!d) return;                                         // refused: exactly what unpack refuses

const nlohmann::json& m = d->skeleton;                  // version, architecture, config, metadata, sample_rate
if (m.value("architecture", "") == "SlimmableContainer")
{
    for (const auto& sub : m.at("config").at("submodels"))   // each submodel: its own config and weights
        if (const std::vector<float>* w = d->weightsOf(sub.at("model")))
            build(sub.at("model").at("config"), *w);
}
else if (const std::vector<float>* w = d->weightsOf(m))     // a plain model; nullptr: it holds no weights
{
    build(m.at("config"), *w);
}

nlohmann::json cfg = m.at("config");                    // .at, not []: [] on a const json is UB for a missing key
d->refill(cfg);                                         // a subtree as .nam JSON, if a loader wants one
```

Refilled whole and dumped, the skeleton is byte for byte what `unpack` returns. Both readers refuse a
skeleton nested deeper than 128 (`namz::kMaxDepth`; real captures nest about 10 deep), so a loader's own
recursive walk over it stays shallow.

The declarations in `namz.h` include `<nlohmann/json.hpp>`, so it must be on the include path of every TU
that includes the header (the `namz::namz` CMake target carries it). Every such TU must see the **same**
nlohmann/json, configured the same way (`JSON_DIAGNOSTICS` and the rest), as the TU that defines
`NAMZ_IMPLEMENTATION`: `decode`'s mangled name carries no nlohmann type, so a mismatch is not a link error —
it links silently against a `Decoded` of another layout. For the same reason every such TU must see the
**same version** of `namz.h` as that one: 4.4.0 added `PackOptions::dtype` and `pack`'s mangled name did
not change, so an older header's `PackOptions` handed to a 4.4.0 `pack` is read past its end — a stray 1
there would write float16 that nobody asked for.

## Use the CLI

```
namz encode <in.nam> <out.namz> [--no-shuffle] [--set key=value ...]
namz encode <in.nam> <out.nam16z> --f16 [--no-shuffle] [--set key=value ...]   # float16, lossy
namz decode <in.namz|in.nam16z> <out.nam>
namz map    <in.namz|in.nam16z> [--json]   # print the metadata header (no weight decode)
namz verify <in.nam> [--f16]          # pack->unpack round-trip check + ratio
```

Float16 is asked for with `--f16`, and the name has to agree: `--f16` writes only a `*.nam16z`, and a
`*.nam16z` is written only with `--f16` — either mismatch is refused before anything is read or written.
The name never picks the dtype by itself. `verify --f16` also prints the largest change of any weight
against the float32 pack.

```console
$ namz encode amp.nam amp.namz --set gear_make="Two Notes" --set device="tube:1,pnp:1"
amp.nam -> amp.namz  (295919 -> 62138 bytes, 21.0%)

$ namz map amp.namz
  device = tube:1,pnp:1
  gear_make = Two Notes

$ namz map amp.namz --json
{"device":"tube:1,pnp:1","gear_make":"Two Notes"}
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The first configure fetches nlohmann/json (pinned). The CLI lands at `build/namz`.

Consume from another CMake project:

```cmake
include(FetchContent)
FetchContent_Declare(namz GIT_REPOSITORY https://github.com/darwinscat/namz.git GIT_TAG v5.0.0)
FetchContent_MakeAvailable(namz)
target_link_libraries(your_target PRIVATE namz::namz)   # header-only + nlohmann/json
```

## Format

The wire format is small and documented in **[NAMZ-FORMAT.md](NAMZ-FORMAT.md)**. It carries a version and
codec/dtype bytes, so it can grow (a `float16` dtype, as of 4.4.0; e.g. a future `zstd` codec) without
breaking old readers: a reader refuses a byte value it does not know.

## Language ports

The format is a **cross-language contract**, not just one binary. Native ports live in this repo and are
**byte-for-byte identical** to this C++ reference — each is validated against the shared
[conformance vectors](conformance/) (the cross-language TCK: golden encode pairs, must-reject blobs,
decode-only blobs with the exact `.nam` each decodes to, and `.nam` inputs encode must refuse) plus a
differential fuzz against the reference, so `encode` reproduces the exact same `.namz`, `decode` is
bit-exact float32 and prints the reference's own bytes everywhere.

| language | package | registry | source |
|---|---|---|---|
| C++ (reference) | `namz.h` | header / CMake | [`include/`](include/) |
| Python | `namz` | [PyPI](https://pypi.org/project/namz/) | [`python/`](python/) |
| JavaScript / TypeScript | `@darwins-cat/namz` | [npm](https://www.npmjs.com/package/@darwins-cat/namz) | [`js/`](js/) |

Each port ships an idiomatic API (`pack` / `unpack` / `readMeta` / `isNamz`) and the same
`encode` / `decode` / `map` / `verify` CLI, uses the language's **native** JSON library, and **versions
independently** (its own semver and tags) while tracking the same wire **format version 2**. See
[docs/LANGUAGE-PORTS-BRIEF.md](docs/LANGUAGE-PORTS-BRIEF.md) for the porting model.

## License

[MIT](LICENSE). namz is by [Darwin's Cat](https://darwinscat.com) — it grew out of the
[OrbitCab](https://github.com/darwinscat/orbitcab) plugin and was split out so anything can use it.
