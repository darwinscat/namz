<!-- SPDX-License-Identifier: MIT -->
# `.namz` — the wire format

A `.nam` file is JSON whose bulk is one or more flat `"weights"` arrays written as full-precision
**decimal strings** (~20 chars per number). The NAM engine loads those weights into `std::vector<float>`
— so the decimals are truncated to **float32 on load anyway**.

`.namz` stores each weight as a 4-byte **float32** instead of ~20 bytes of text: **≈5.5× smaller** than
the raw JSON, and **bit-exact** to what the engine computes. Everything except the weight arrays
(architecture / config / metadata) is preserved verbatim in a JSON *skeleton*.

Reference implementation: [`include/namz.h`](include/namz.h). Tests: [`tests/roundtrip.cpp`](tests/roundtrip.cpp).

## Wire format

All multi-byte integers are **little-endian** (written explicitly, so the container is endian-portable).
The float32 payload is stored in host byte order — every real NAM target, and NAM itself, is
little-endian IEEE-754.

```
[0..3]   magic          'N','A','M','Z'
[4]      formatVersion  2   (readers still accept 1 = no meta block)
[5]      codec          0 = store/uncompressed;       1 = deflate (reserved), 2 = zstd (reserved)
[6]      dtype          0 = float32;                  1 = float16 (reserved, lossy)
[7]      flags          bit0 = weight bytes shuffled into 4 byte-planes (lossless; groups the
                               structured bytes so an OUTER compressor squeezes ~6% more)
[8..9]   metaLen        u16 — bytes of the display-metadata JSON that follows (v2 only)
[meta]   metaLen bytes  small JSON of display fields — read via readMeta() WITHOUT touching the weights
[..]     body (codec 0 = stored verbatim):
             u32  skeletonLen
             u8   skeleton[skeletonLen]   minified JSON; each numeric "weights" array is replaced by
                                          its ordinal integer index
             u32  numArrays
             u32  lengths[numArrays]      float count of each weights array (in index order)
             u8   payload[]               sum(lengths) * 4 bytes of float32 (byte-shuffled iff flags bit0)
```

`unpack` un-shuffles the payload, rebuilds the JSON (weights re-inserted as float32 numbers) and hands it
to the usual `parse → NAM engine` path, so the loaded model is identical.

## Contracts (enforced by the tests)

- **Lossless to float32** — for every weight, `unpack(pack(x)) == (float)x`, bit-exact, at any nesting
  depth (incl. `SlimmableContainer` submodels). Verified across `-0.0`, subnormals, and `FLT_MAX`.
- **Metadata/config preserved** verbatim — nested objects, `null`, unicode, big doubles, escapes.
- **Deterministic** — `pack(x)` is byte-identical across runs and platforms (no compressor, so no
  zlib/gzip header variation); the codec is idempotent (`pack(unpack(pack(x))) == pack(x)`).
- **Robust** — `unpack` never crashes/hangs/OOMs; every malformed input (bad magic, unknown
  version/codec/dtype, truncation at any byte, a lying `metaLen`, a huge `numArrays`, an oversized body
  over the output cap) is rejected cleanly and returns empty. Because the body is stored (no self-healing
  compressed stream), **any** truncation or trailing junk fails the exact-length check.
- **Compatible** — `formatVersion 1` blobs (no meta block) still unpack; `readMeta` returns empty for
  v1 / non-`.namz`, and the typed display fields for v2.

> Out of contract: non-finite weights (`NaN`/`±Inf`). Real NAM captures never contain them; JSON can't
> represent them either. The codec targets the finite float32 range NAM produces.

## The metadata header

`pack` can stamp/overwrite display fields (typed: `true`/`false` → bool, all-digits → int, else string).
The same set is mirrored into the small header block so a reader can pull it cheaply (`readMeta`) — no
weight decode. Conventional fields:

| field | example | used for |
|---|---|---|
| `modeled_by` | `Darwin's Cat` | provenance / tooltip |
| `gear_make` / `gear_model` | `Two Notes` / `ReVolt Guitar` | a caption |
| `gear_type` | `preamp` | — |
| `tone_type` | `clean` / `crunch` / `hi-gain` | channel labels |
| `boost` | `true` | a boost indicator |
| `device` | `tube:1,pnp:1` | schematic device glyphs |

**`device` spec** — a comma-separated list of `type:count` in signal order; supports hybrids: `tube`,
`pnp`/`npn`/`bjt`/`transistor`, `fet`/`jfet`/`mosfet`, `dsp`/`chip`/`ic`, `diode`. Examples: `tube:4`,
`pnp:1`, `tube:1,pnp:1` (a tube + a transistor).

### Capture-identity keys (knob/switch positions)

A capture tool that models one hardware device across MANY knob/switch positions (OrbitNamCapture)
stamps each file's position into the header, so a player can build its device selector from metadata
instead of parsing filenames. All keys are flat strings (the header's contract):

| field | example | meaning |
|---|---|---|
| `settings.<control>` | `settings.gain` = `12h` | THIS file's position of one control |
| `controls` | `channel:channel=green\|orange\|red; boost:boost=off\|on; gain:gain=07h\|08h\|…\|17h` | the whole device's control spec |
| `rig_id` | `dc-revolt-guitar` | stable device identity — grouping survives display renames |
| `slot` | `preamp` | where the device sits in a player's chain: `pedal` · `preamp` · `amp` · `poweramp` · `rig` |

**`controls` spec** — `;`-separated entries of `<name>:<role>=<value>\|<value>\|…` in capture order.
Roles mirror the filename-token grammar: `channel` (colour word or `chN`), `gain` (`NNh` clock
positions), `boost` (presence token; `settings.<boost control>` is `on`/`off` and the conventional
`boost` bool is stamped alongside), `topology` (`PP`/`SE`), `generic` (a free token). A family of
files sharing `rig_id` (else `gear_model`) + `controls` IS one device; each file's `settings.*`
places it in that device's matrix.

## The `.orbitrig` pack

A **device pack** ships a whole modeled rig as one unit — a folder (for editing/sync) or, for
sharing, a `.orbitrig.zip` (zip the `.namz` together: the byte-plane shuffle pays off across the
family, which is why the codec stores rather than deflates each file). A player loads it by
drag-dropping the zip or the folder, or via an "Import" action.

```
ReVolt Guitar.orbitrig/            (share as ReVolt Guitar.orbitrig.zip)
  rig.json                         the manifest — chain + controls + file index + EQ hints
  ReVolt-green-07h.namz            models (codec=store), one per knob/switch combination
  ...
```

**`rig.json`** is the pack's source of truth; each `.namz` still carries its own
`settings.*`/`controls`/`rig_id`/`slot` header, so a single file pulled out of the pack still knows
what it is (a player with no manifest falls back to scanning `.namz` + reading those headers).

```json
{
  "format": "orbitrig", "schema": 1,
  "rig_id": "dc-revolt-guitar", "name": "ReVolt Guitar Stack", "modeled_by": "Darwin's Cat",
  "chain": [
    { "kind": "nam", "slot": "preamp",
      "gear": { "make": "Two Notes", "model": "ReVolt Guitar", "type": "pedal" },
      "controls": [
        { "name": "channel", "role": "channel", "values": ["green","orange","red"] },
        { "name": "boost",   "role": "boost",   "values": ["off","on"] },
        { "name": "gain",    "role": "gain",    "values": ["07h","...","17h"] }
      ],
      "files": [
        { "file": "ReVolt-green-07h.namz", "settings": {"channel":"green","boost":"off","gain":"07h"} }
      ]
    }
  ]
}
```

- **`chain`** — stages in signal order. A stage has a `kind`: `nam` (a captured non-linear device),
  `ir` (a linear cabinet impulse — future), or `eq` (a software tone stack — see below). A player
  that meets a `kind` it doesn't know **skips that stage** rather than failing, so new stage types
  never break old players. `slot` (`pedal`/`preamp`/`amp`/`poweramp`/`rig`) routes the stage.
- **`files`** — the authoritative list for a `nam` stage: each entry is a model + its `settings`.
  (A manifest-less folder is read by scanning `*.namz` instead.)
- **`eq` stage** — the tone stack is ALWAYS software; this stage is optional author guidance for the
  player's EQ, never captured audio. Fields (all optional): `model` (e.g. `fmv`), `defaults` (a
  starting value for any EQ knob), `hidden` (knobs to hide, e.g. `["hpf","lpf"]`), `tone_only` (a
  single simplified "TONE" knob instead of the full EQ), `show_curve` (draw the EQ response or not).

Additive keys never bump `schema`; a bump is reserved for an incompatible change (avoided by
design). The reference reader/selector is `namz::rig`.

## Versioning

`formatVersion`, `codec`, and `dtype` are single bytes with reserved values, so the format can grow
without breaking old readers: a future `zstd` codec (2) or `float16` dtype (1) would bump those bytes, and
a reader that doesn't understand them refuses cleanly rather than mis-decoding.
