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
- **Fast to load** — no decompression; just un-shuffle bytes and re-emit JSON.
- **Cheap metadata** — a small header block (tone / device / gear / …) is readable **without** decoding
  the weights.

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

`pack`/`unpack`/`readMeta` are byte-in / byte-out — no JUCE, no framework — so the same header drops into
a plugin, a tool, or a script.

## Use the CLI

```
namz encode <in.nam> <out.namz> [--no-shuffle] [--set key=value ...]
namz decode <in.namz> <out.nam>
namz map    <in.namz> [--json]        # print the metadata header (no weight decode)
namz verify <in.nam>                  # pack->unpack round-trip check + ratio
```

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
FetchContent_Declare(namz GIT_REPOSITORY https://github.com/darwinscat/namz.git GIT_TAG v0.1.0)
FetchContent_MakeAvailable(namz)
target_link_libraries(your_target PRIVATE namz::namz)   # header-only + nlohmann/json
```

## Format

The wire format is small and documented in **[NAMZ-FORMAT.md](NAMZ-FORMAT.md)**. It carries a version and
reserved codec/dtype bytes, so it can grow (e.g. a future `zstd` codec or `float16` dtype) without breaking
old readers.

## License

[MIT](LICENSE). namz is by [Darwin's Cat](https://darwinscat.com) — it grew out of the
[OrbitCab](https://github.com/darwinscat/orbitcab) plugin and was split out so anything can use it.
