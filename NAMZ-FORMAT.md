<!-- SPDX-License-Identifier: MIT -->
# `.namz` — the wire format

A `.nam` file is JSON whose bulk is one or more flat `"weights"` arrays written as full-precision
**decimal strings** (~20 chars per number). The NAM engine loads those weights into `std::vector<float>`
— so the decimals are truncated to **float32 on load anyway**.

`.namz` stores each weight as a 4-byte **float32** instead of ~20 bytes of text: **≈5.5× smaller** than
the raw JSON, and **bit-exact** to what the engine computes. Everything except the weight arrays
(architecture / config / metadata) is preserved verbatim in a JSON *skeleton*.

On request the weights can be stored as IEEE **float16** instead — half the payload, and **lossy**: see
"The float16 dtype (`.nam16z`)". Everything this document calls lossless or bit-exact is float32's.

Reference implementation: [`include/namz.h`](include/namz.h). Tests: [`tests/roundtrip.cpp`](tests/roundtrip.cpp).

## Wire format

All multi-byte integers are **little-endian** (written explicitly, so the container is endian-portable).
The float32 payload is stored in host byte order — every real NAM target, and NAM itself, is
little-endian IEEE-754. A float16 is little-endian, written byte by byte.

```
[0..3]   magic          'N','A','M','Z'
[4]      formatVersion  2   (readers still accept 1 = no meta block; every other value, 0 included, is refused)
[5]      codec          0 = store/uncompressed;       1 = deflate (reserved), 2 = zstd (reserved)
[6]      dtype          0 = float32;                  1 = float16 (lossy, written only on request; 4.4.0)
                        every other value is refused
[7]      flags          bit0 = weight bytes shuffled into byte-planes, one per byte of a weight: 4 for
                               float32, 2 for float16 (lossless; groups the structured bytes so an
                               OUTER compressor squeezes ~6% more on float32, ~4% on float16)
[8..9]   metaLen        u16 — bytes of the display-metadata JSON that follows (v2 only)
[meta]   metaLen bytes  small JSON of display fields — read via readMeta() WITHOUT touching the weights
[..]     body (codec 0 = stored verbatim):
             u32  skeletonLen
             u8   skeleton[skeletonLen]   minified JSON; each numeric "weights" array is replaced by
                                          its ordinal integer index
             u32  numArrays
             u32  lengths[numArrays]      float count of each weights array (in index order)
             u8   payload[]               sum(lengths) * 4 bytes of float32, or sum(lengths) * 2 bytes of
                                          float16 (byte-shuffled iff flags bit0)
```

| dtype | stored weight | bytes | shuffled payload (flags bit0) | lossless |
|---|---|---|---|---|
| 0 | IEEE-754 binary32, host order (little-endian) | 4 | plane 0 = every weight's byte 0, … plane 3 = byte 3 | yes — the engine's float32 |
| 1 | IEEE-754 binary16, little-endian | 2 | plane 0 = every weight's low byte, then plane 1 = every high byte | **no** — rounded to half |

`unpack` un-shuffles the payload, rebuilds the JSON (weights re-inserted as float32 numbers) and hands it
to the usual `parse → NAM engine` path, so the loaded model is identical. That path prints every weight
as decimal text only for the loader to parse it back; `decode` stops before the text. It parses the
skeleton as stored — each placeholder still an integer — and un-shuffles the payload straight into one
float32 vector per segment, so a loader that builds its model itself reads the architecture and config
from the skeleton and takes each model's weights from the segment its placeholder names. Filling every
placeholder with its segment and dumping the tree gives exactly the bytes `unpack` returns.

## The float16 dtype (`.nam16z`)

Dtype 1 stores each weight as an IEEE-754 binary16 ("half"): 2 bytes instead of 4. It exists for a
download where bytes matter more than the last bits — a web page fetching captures — while plugins and the
`.namz` downloads keep float32. It is **lossy**, and so it is written only when asked for
(`PackOptions::dtype = DtypeF16`, the CLI's `--f16`); float32 stays the default, byte for byte what 4.3.1
wrote.

- **Rounding.** The half is the one nearest the weight's **float32** — the same float32 a dtype-0 file
  stores — round to nearest, ties to even. So packing a `.nam` as float16 and re-packing as float16 the
  `.nam` a float32 `.namz` unpacks to give the same file. A float32 whose half is subnormal keeps it as a
  half subnormal (step 2^-24); one below 2^-25 is a zero of its sign, and ±2^-25 itself is a tie that goes
  to the zero of its sign (−2^-25 → `0x8000`, the sign is kept).
- **Range.** `pack` refuses the whole `.nam` (writes nothing) if any weight's float32 is not finite, or its
  half would overflow: |w| ≥ 65520, the tie above the largest half 65504 rounding to the even infinity.
  Up to there a weight rounds to 65504 at most. The 230 real captures measured for 4.4.0
  stay below 3 (largest |w| 2.65).
- **Bytes.** Each half is little-endian. Unshuffled, the payload is the halves in order, low byte first;
  shuffled (flags bit0), it is every weight's low byte, then every weight's high byte.
- **Reading.** `unpack` and `decode` go by the dtype byte, never by a file name. Each half widens to the
  float32 of the same value — exact, every half has one — so `decode`'s segments are `std::vector<float>`
  as for float32, and `unpack` prints those float32 values as it prints a float32 file's. Every reader
  rule holds unchanged, with the payload `sum(lengths) * 2` bytes exactly: truncation at any byte, trailing
  bytes, lengths that do not sum to the payload, a payload half that is an infinity or NaN, the caps
  (`decode`'s `maxFloats` counts weights, whichever their width). `unpack` and `decode` keep one verdict.
- **Everything else is the same bytes.** Magic, version, codec, flags, the meta block and the skeleton
  are what the float32 file of the same `.nam` holds; only the dtype byte and the payload differ.
- **The name.** A float16 file is conventionally named `.nam16z`, so the loss shows in the name. The CLI
  holds to it: `--f16` writes only a `*.nam16z`, and a `*.nam16z` only with `--f16`.
- **What it costs.** Each weight moves by at most half a step of half's 11-bit significand — 2^-11 of
  its magnitude — or by 2^-25 at most below 2^-14. On the real captures this was measured at 58 dB or more under the
  output and 32–41 dB under each capture's own training error. The files are 0.55× the float32 `.namz`,
  0.52× after `brotli -q 11`; the 2-plane shuffle takes another ~4% off the brotli size.

The C++ reference writes and reads dtype 1 as of 4.4.0, and both language ports as of their 1.3.0
(`@darwins-cat/namz` and `namz` on PyPI). The conformance vectors pin it: float16 goldens (`.nam16z`),
decode-only halves, and the refusals of both readers and of `pack`. The vector `bad-dtype` — a float32 file
with its dtype byte set to 1 — is refused by the length rule (its 4-byte payload is twice what its lengths
ask of 2-byte halves); `bad-dtype-2` is the unknown dtype.

## Placeholders and depth

These rules decide what a reader accepts beyond the byte layout. They are stated here exactly, for a port to
follow word for word; `unpack` and `decode` share all of them.

**Placeholder.** A placeholder is an object member, at any depth of the skeleton, named exactly `weights`,
whose value is an integer literal — no fraction, no exponent — that fits a signed or an unsigned 64-bit
integer. `-0` is one, and names segment 0. A placeholder names segment `value` and must satisfy
`0 ≤ value < numArrays`; `unpack` and `decode` both refuse the file otherwise. So `9223372036854775808`
(it fits an unsigned 64-bit integer) is a placeholder naming no segment, and the file is refused. Any other
`weights` value is not a placeholder and stays the number it is: `1.0` has a fraction, `0e0` an exponent,
`18446744073709551616` is past every 64-bit integer and reads as a double; the reference prints them back
as `1.0`, `0.0` and `1.8446744073709552e+19`. A segment no placeholder names is allowed; a segment named
by several placeholders fills each of them. Two `weights` keys in one object are one member: the last one
counts, as in the reference's JSON library.

A placeholder is `pack`'s to write, never the input's: `pack` refuses a `.nam` in which a member named
`weights`, at any depth, holds an integer literal by the rule above — the metadata `pack` stamps in
included. Stored, it would be a placeholder: `"config":{"weights":0}` would read back as segment 0's
array, and `"weights":7` would name a segment that does not exist, so both readers would refuse the file
`pack` just wrote. A `weights` value that is no integer literal (`1.0`, `0e0`, a string, an object, an
array that is not all numbers) is packed as the value it is.

**Depth.** A scalar has depth 0; an array or object has 1 + the greatest depth of its members, so an empty
one has depth 1. Depth is counted **as written**: on the text, before it is parsed, by the brackets outside
strings — so a member a later duplicate key replaces still counts (`{"x":<128 arrays around 0>,"x":0}` is
129 deep, whichever `x` a parser keeps). The limit is **128** (`namz::kMaxDepth`): `unpack` and `decode`
refuse a skeleton deeper than 128; `pack` refuses a `.nam` deeper than 128, so everything it writes reads
back; `readMeta` returns no metadata for a meta block deeper than 128 (the meta block takes no part in
`unpack`'s or `decode`'s verdict). Real captures nest about 10 deep. The limit is what keeps every recursive
walk over the tree — the reader's own, a JSON library's print and compare, a loader's model builder — off
the end of the stack.

**Finite weights.** Every float the payload stores is a finite float32 (a finite half, for dtype 1).
`unpack` and `decode` refuse a file whose payload holds a NaN or an infinity, whether a placeholder names
its segment or not; `pack` refuses a `.nam` holding a weight whose float32 is not finite — `1e39` is a
finite JSON number past the float32 range (`3.4028235e38` rounds to `FLT_MAX` and is kept) — and, for
dtype 1, one whose half overflows (|w| ≥ 65520).

**JSON text.** Every JSON text — the `.nam`, the skeleton, the meta block — is UTF-8, one leading byte-order
mark skipped; UTF-16 or UTF-32 is not JSON text here. It is refused wherever the reference's JSON library
(nlohmann/json 3.11) refuses it, a value a later duplicate key replaces included: an escaped lone surrogate
(`"\ud800"`), a number past every double (`1e400`, an integer literal of 400 digits). The reference prints
a double with that library's digits — Grisu2, which is neither always the shortest nor always the closest
(`-198.70986938476562` prints as `-198.70986938476563`) — in fixed form while the decimal point sits within
15 digits of the first one, else as `d.ddde±XX` (`1e+15`, `1e-05`, `1.8446744073709552e+19`). A port prints
the same bytes; the shortest round-trip digits of its own language are not them. `readMeta` prints an
integer as its digits, signed or unsigned, any other number as C's `"%f"` (six decimals, an exact tie to
even), and an array or object as the reference prints it.

The C++ reference states the placeholder and depth rules as of 4.3.0, and all of them as of 4.3.1; as of
4.3.1 the conformance vectors pin them, the refusals of a `.nam` that holds a placeholder and of
`formatVersion 0` included, and both language ports follow them (`@darwins-cat/namz` and `namz` on PyPI,
1.2.0).

## Contracts (enforced by the tests)

- **Lossless to float32** — for every weight, `unpack(pack(x)) == (float)x`, bit-exact, at any nesting
  depth up to 128 (incl. `SlimmableContainer` submodels). Verified across `-0.0`, subnormals, and `FLT_MAX`.
  This is dtype 0's contract. Dtype 1 has its own: every weight reads back as exactly the half nearest
  `(float)x`, ties to even — checked against numpy.float16 and against a rounding by arithmetic, on every
  half, every tie between two halves and its neighbours, and every weight of the real captures.
- **Metadata/config preserved** verbatim — nested objects, `null`, unicode, big doubles, escapes.
- **Deterministic** — `pack(x)` is byte-identical across runs and platforms (no compressor, so no
  zlib/gzip header variation); the codec is idempotent (`pack(unpack(pack(x))) == pack(x)`), with either
  dtype. The half rounding is done on the bits, so no floating-point mode or hardware half support moves it.
- **Robust** — `unpack`, `decode`, `readMeta` and `pack` never crash, hang or run out of memory on any
  input, the stack included: memory is linear in the input and bounded by the caps, and no walk recurses
  deeper than 128. Every malformed `.namz`, of either dtype (bad magic, unknown version/codec/dtype,
  truncation at any byte, a payload that is not `sum(lengths)` weights of the file's dtype,
  a lying `metaLen`, a huge `numArrays`, lengths whose sum overflows — a 32-bit `size_t` included — a
  placeholder that names no segment, a skeleton nested deeper than 128, a payload float that is NaN or
  infinite, a body over the output cap) is refused cleanly: `unpack` returns empty, `decode` nothing.
  `readMeta` returns no metadata for a block it cannot read; `pack` returns empty for a `.nam` that is not
  JSON, nests deeper than 128, holds an integer `weights` member, or holds a weight whose float32 is not
  finite. Because the body is stored (no self-healing compressed stream), **any**
  truncation or trailing junk fails the exact-length check.
- **Two readers, one verdict** — `decode` refuses exactly the inputs `unpack` refuses, its size cap aside:
  `unpack` caps the JSON it would print, `decode` the floats — those the file stores, and those its
  placeholders name, a segment named twice counted twice (the one way a small file asks for a large
  model). Where both accept, `decode` refilled and dumped is `unpack`'s output, byte for byte.
- **Compatible** — `formatVersion 1` blobs (no meta block) still unpack; `readMeta` returns empty for
  v1 / non-`.namz`, and the typed display fields for v2. Only 1 and 2 are defined: a `formatVersion` of 0
  is refused like any unknown one.

> Refused: non-finite weights (`NaN`/`±Inf`), on the way in and on the way out (see "Finite weights").
> Real NAM captures never contain them; JSON can't represent them either. The codec targets the finite
> float32 range NAM produces.

## The metadata header

`pack` can stamp/overwrite display fields (typed: `true`/`false` → bool, all-digits → int, else string).
A **leading zero keeps the value a string**: `0012345` is an identifier — a serial number, a catalogue
code — and the zeros in front of it are part of what is stamped, so typing it to `12345` would hand the
reader back a different string than the writer put in. JSON says as much about its own literals (a
number may not start with a redundant zero); bare `0` is the quantity zero and stays one.

The same set is mirrored into the small header block so a reader can pull it cheaply (`readMeta`) — no
weight decode. Conventional fields:

| field | example | used for |
|---|---|---|
| `modeled_by` | `Darwin's Cat` | provenance / tooltip |
| `gear_make` / `gear_model` | `Two Notes` / `ReVolt Guitar` | a caption |
| `gear_type` | `preamp` | — |
| `gear_year` | `1979` | the year THIS box was built |
| `gear_serial_number` | `0012345` | the unit, not the family — a string, zeros and all |
| `gear_url` | `https://…` | the box's page (`https://` only, or a reader drops it) |
| `designed_in` / `made_in` | `Japan` / `Taiwan` | where it was drawn up / screwed together |
| `tone_type` | `clean` / `edge` / `crunch` / `hi-gain` | how much gain this sounds like |
| `voicing` | `od` / `dist` / `fuzz` | what KIND of dirt (absent = unstated) |
| `boost` | `true` | a boost indicator |
| `device` | `tube:1,pnp:1` | schematic device glyphs |

**`device` spec** — what is in the captured signal path: a comma-separated list of `type:count` in
signal order, so hybrids state both halves. Examples: `tube:4`, `bjt:4`, `tube:1,bjt:1` (a tube AND a
transistor), `ic:1,diode:2` (an op-amp that amplifies + the diodes that clip). The vocabulary is
CLOSED, one spelling each:

| token | element |
|---|---|
| `tube` | a valve |
| `bjt` | a bipolar transistor (`pnp`/`npn` are read as this too — legacy captures) |
| `fet` | a field-effect transistor |
| `ic` | an ANALOGUE op-amp: it sits in the audio path |
| `dsp` | a digital signal path — an honest warning that this capture is of a model |
| `diode` | the clipper itself |

`ic` and `dsp` are deliberately distinct: a Tube Screamer's op-amp and a digital multi-effect share
nothing but a DIP package, and which one a pedal has is the most useful thing this row can say.
A player renders one glyph per element; the same string appears in `rig.json` as the stage's
`circuit` (a stage's `device` is already its selectable matrix).

**`tone_type` scale** — `clean < edge < crunch < hi-gain`, ORDERED, and it measures gain amount only.
A device-level value (in `rig.json`) describes the DEFAULT position — mid-sweep gain, the combination
a player loads — so the label matches what you hear when you pick the device; per-file values may
refine it. Treat the list as closed — players hardcode the order, so a value inserted later silently
changes every existing filter.

**`voicing`** is the SECOND, independent axis: `od` (overdrive), `dist` (distortion), `fuzz` — what
KIND of dirt, not how much. It exists because a fuzz and a metal distortion sit at the same
`tone_type` and sound nothing alike: folding that into the gain scale would destroy the ordering the
scale exists for AND lose the distinction. Unordered, and absent is a normal answer — a clean preamp
has no voicing. Era words ("modern", "vintage") belong to neither axis and are absent from both.

### Capture-identity keys (knob/switch positions)

A capture tool that models one hardware device across MANY knob/switch positions (OrbitNamCapture)
stamps each file's position into the header, so a player can build its device selector from metadata
instead of parsing filenames. All keys are flat strings (the header's contract):

| field | example | meaning |
|---|---|---|
| `settings.<control>` | `settings.gain` = `150` | THIS file's position of one control |
| `controls` | `channel:channel=green\|orange\|red; boost:boost=off\|on; gain:gain=0\|30\|…\|300` | the whole device's control spec |
| `sweep.<control>` | `sweep.gain` = `300` | how far that DIAL turns, degrees (absent = not a dial) |
| `rig_id` | `dc-revolt-guitar` | stable device identity — grouping survives display renames |
| `slot` | `preamp` | where the device sits in a player's chain: `pedal` · `preamp` · `amp` · `poweramp` · `rig` |

**`controls` spec** — `;`-separated entries of `<name>:<role>=<value>\|<value>\|…` in capture order.
Roles: `channel`, `gain`, `boost` (`settings.<boost control>` is `on`/`off`, and the conventional
`boost` bool is stamped alongside), `topology`, `generic`. A family of files sharing `rig_id` (else
`gear_model`) + `controls` IS one device; each file's `settings.*` places it in that device's matrix.

**Knob positions are DEGREES** — a bare integer, `0`…`sweep`. Not clock hours: degrees place a knob
on a dial without a lookup table, and `sweep.<control>` says how far that dial goes, so `150` on a
`300°` dial is unambiguously the middle. A dial's default (what a player loads, and what `tone_type`
describes) is the captured position nearest mid-sweep.

**A filename means nothing.** It is an id the player echoes back and a caption it may show; nothing
parses it. Two sources of truth for one fact is one too many, and the copy you cannot fix later is
the one already written on someone's disk. Files with no `controls` key are not analysed for hints —
they are simply entries with no knobs. (Earlier versions of this format did parse names for
colour/`chN`, `NNh` gain, `boost` and `PP`/`SE`. That grammar is gone.)

## The `.orbitrig` pack

A **device pack** ships a whole modeled rig as one unit — a folder (for editing/sync) or, for
sharing, a `.orbitrig.zip` (zip the `.namz` together: the byte-plane shuffle pays off across the
family, which is why the codec stores rather than deflates each file). A player loads it by
drag-dropping the zip or the folder, or via an "Import" action.

```
ReVolt Guitar.orbitrig/            (share as ReVolt Guitar.orbitrig.zip)
  rig.json                         the manifest — chain + controls + tone + file index + EQ hints
  m01.namz                         models (codec=store), one per knob/switch combination
  device.webp                      the box's photograph (optional) — see `picture` below
  ...
```

**`rig.json`** is the pack's source of truth; each `.namz` still carries its own
`settings.*`/`controls`/`rig_id`/`slot` header, so a single file pulled out of the pack still knows
what it is (a player with no manifest falls back to scanning `.namz` + reading those headers).

The reference writer lays the manifest out the way the example below is laid out — keys in the order
this document lists them, a control, a file entry, a band or a position on one line, long arrays
wrapped under their first element — so a person can read a pack without a tool. That is a courtesy
of the writer, not a rule of the format: a reader parses JSON and must not depend on a line.

```json
{
  "format": "orbitrig", "schema": 5,
  "rig_id": "dc-ts9", "name": "Golden Drive", "modeled_by": "Darwin's Cat",
  "picture": "device.webp", "url": "https://darwinscat.com/packs/golden-drive",
  "chain": [
    { "kind": "nam", "slot": "pedal",
      "gear": { "make": "Darwin's Cat", "model": "Golden Drive", "type": "pedal",
                "year": 1979, "serial_number": "0012345",
                "designed_in": "Japan", "made_in": "Japan",
                "url": "https://darwinscat.com/gear/golden-drive" },
      "tone_type": "crunch",
      "input_db": -3.0, "output_db": -12.0,
      "controls": [
        { "name": "gain", "role": "gain", "sweep": 300, "values": ["0","150","300"] }
      ],
      "tone": [
        { "name": "tone", "sweep": 300, "placement": "post", "reference": "300",
          "operating_point": { "gain": "150" },
          "grid": { "f_lo": 20.0, "f_hi": 20000.0, "points": 8 },
          "trusted": { "lo_hz": 53.7, "hi_hz": 7455.2, "lo_index": 1, "hi_index": 6, "span_db": 24.0, "levels": 5 },
          "positions": [
            { "value": "0",   "norm": 0.0, "db": [-0.2, -0.2, -0.4, -0.9, -3.2, -9.4, -16.8, -24.1] },
            { "value": "300", "norm": 1.0, "db": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0] }
          ] },
        { "name": "bass", "sweep": 300, "placement": "post", "reference": "150",
          "operating_point": { "gain": "150" },
          "sections": [
            { "type": "low_shelf", "hz": 167, "q": 0.58, "range_db": [-12.0, 6.0],
              "hz_at": [120, 240], "q_at": [0.5, 0.7] }
          ] }
      ],
      "files": [
        { "file": "m01.namz", "settings": { "gain": "0" } }
      ]
    }
  ]
}
```

- **`chain`** — stages in signal order. A stage has a `kind`: `nam` (a captured non-linear device),
  `ir` (a linear cabinet impulse — future), or `eq` (a software tone stack — see below). A player
  that meets a `kind` it doesn't know **skips that stage** rather than failing, so new stage types
  never break old players. `slot` (`pedal`/`preamp`/`amp`/`poweramp`/`rig`) routes the stage.
  - `input_db` — the pack's working point: how much softer or harder the guitar reaching THIS WHOLE
    device is, signed dB, absent = 0. It changes the SOUND and not the volume — less signal into a
    non-linear network is less drive — and a player applies it before anything splits, the dry side of
    a `blend` knob included: there is one guitar, and it cannot arrive at the two ends of a mix at two
    different levels. It ADDS to `files[].input_db`, which keeps the meaning it has below — the trim of
    one alias against its neighbour. What reaches the model is the two of them together.
  - `output_db` — the level this pack leaves at, signed dB, absent = 0, so packs can be balanced
    against one another: a Big Muff comes out some 12 dB louder than a clean preamp, and a boost is
    hotter still than the preamp it feeds. Volume only, and applied LAST in the stage — AFTER the
    dry/wet mix. One number for the whole stage: applied before the mix it would ride the wet side
    alone and move a blend the player was told to hold.

  Neither key bumps `schema`, which stays 4. They are additive, so a reader that has not met them
  plays exactly as it plays today — that is the status quo and not a regression. Packs written before
  them, where a device's level was spread across `files[].input_db`, keep playing correctly for the
  same reason: a reader sums the stage level with the file level, and in those packs the stage level
  is simply absent.
- **`files`** — the authoritative list for a `nam` stage: each entry is a model + its `settings`.
  (A manifest-less folder is read by scanning `*.namz` instead.) An entry may also carry:
  - `input_db` — how much SOFTER the signal reaching this model is, dB (absent = 0). A device may
    point two settings at one file — the bottom notches of a gain dial, where the hardware gives hiss
    and little else, played through the neighbour above them with less going in. Into the model, not
    out of it: less signal is also less drive, so the dial fades instead of falling into a hole. The
    capture side decides this and writes it down; a player only applies it.
  - `lag_samples` — how many samples LATER than the stage's first file this model's output arrives on
    the same input, at the file's own sample rate — an integer, negative = earlier. Two captures of
    one device come out of training a few samples apart, and a player that crossfades them as they are
    combs. The capture side measures this once, with every model in hand, and writes it on every entry
    it measured (the first file's own is 0); a player delays each model by the largest lag minus its
    own, so any pair sums in phase. Absent = not measured: a player that crossfades must measure for
    itself, and a stage where some entries carry it and some do not is not measured either.
  - `input_lag_samples` — how many samples LATER than its own INPUT this model's output arrives, at the
    file's own sample rate: an integer, negative = earlier, at most 48000 either way. ABSOLUTE, where
    `lag_samples` is relative to the stage's first file — so each entry stands alone, and a stage where
    some carry it and some do not is measured exactly where it says so. Both keys may be present, and
    neither is computed from the other: each answers its own question and is the authority for it. The
    capture side measures it once, from the sweep it shoots beside each take (the impulse response of the
    model against the sweep it was fed; its leading edge is the lag). Absent = not measured. A value of
    the wrong kind (`12.5`, `"12"`) or past the bound is not a statement, and a reader drops it: a model a
    second late is not a latency but a broken capture, and a player that believed the number would
    allocate the delay line it asks for.
  - `polarity` — whether this model's output is INVERTED against that input: `1` in phase, `-1`
    inverted, as a JSON integer. Nothing else is a reading, and a reader drops anything else (`0`, `2`,
    `-1.0`, `"-1"`). Measured with `input_lag_samples`, from the same impulse response — the sign of its
    leading edge — and absent = not measured. Not the `polarity` of a `blend`: that one is the box's own
    dry path against the model, and this one is the model against what it was fed.

    A player that mixes the clean input with the model itself — a bass crossover, clean lows under a
    driven top — needs both: it delays the clean by `input_lag_samples` (converted to its own rate when the
    model runs at another; a negative lag delays the model instead) and multiplies it by `polarity`, and
    the two sum in phase across the crossover. Without them it must measure for itself, or not offer the
    mix. A player that does not mix reads neither. Both keys are additive; on their own they would not
    move `schema`.
  - `epochs` / `esr` — how this model was trained: the number of epochs its weights have (a
    non-negative integer) and the ESR — error-to-signal ratio, lower is closer — the trainer reported
    for it (a number, 0 or more). Written by the capture side from its own records, for a person reading
    the pack; a player plays the same either way. Each is independent, and absent = not known. A value
    of the wrong kind (`800.5` epochs, a negative ESR, a string) is not a statement: a reader drops it.
    Additive — `schema` stays 4.
- **`picture`** — the file name, in the pack root, of the device's photograph (`"device.webp"`), for
  players that draw the box they are playing: a cut-out — background removed, alpha kept — scaled and
  encoded (WebP) by the capture side at export; a player only draws it. Optional and additive:
  absent = the pack ships no picture, and a reader that does not know the key plays as before
  (`schema` stays 4).
- **`url`** — the PACK's page: where a player can send somebody to hear it, read about it or find the
  packs beside it. Not the device's page — that one belongs to the box and lives in `gear.url`; a
  reader handed a single link cannot tell "buy this pedal" from "get this pack", and the two are drawn
  in different places. **Absolute `https://` only, and a reader MUST drop anything else** (a pack is a
  file from a stranger, and a player that follows a `file://` or a `javascript:` out of one has handed
  that stranger the machine). Optional and additive.
- **`gear`** — the box this stage was captured from. `make` / `model` / `type` are its caption, and
  the rest is the PARTICULAR unit rather than the family: `year` (a JSON integer — the year it was
  built), `serial_number` (a **string**: an identifier, so `"0012345"` keeps its zeros and is never
  written as a number), `designed_in` and `made_in` (two fields, not a sentence — a reader that wants
  "Made in Japan" and one that wants "designed in France, made in China" need the same data shaped
  differently, and for gear the pair is often the variant itself), and `url` (the box's own page,
  under the same `https://`-only rule as the pack's). All optional and additive; absent means the pack
  did not say, never "unknown".
- **`tone`** — knobs that are NOT model axes: see below.
- **`blend`** — dry/wet mix knobs, the third kind of control: see below.
- **`eq` stage** — the tone stack is ALWAYS software; this stage is optional author guidance for the
  player's EQ, never captured audio. Fields (all optional): `model` (e.g. `fmv`), `defaults` (a
  starting value for any EQ knob), `hidden` (knobs to hide, e.g. `["hpf","lpf"]`), `tone_only` (a
  single simplified "TONE" knob instead of the full EQ), `show_curve` (draw the EQ response or not).

### `tone` — linear knobs the capture never turned into axes

A tone control after the clipper is **linear**: turning it changes the frequency response, not the
distortion. Capturing it as a matrix axis would multiply the take count for nothing — a 6-position
tone knob turns 21 takes into 126. So the capture tool measures it instead: one short sweep per
position at a fixed operating point, deconvolved against a reference position. The pack ships what it
found, the player applies it as DSP, and the knob it draws is CONTINUOUS — the whole point of
measuring rather than capturing.

Format invariant: **every file of the stage was captured with each tone control at its
`reference` position**, and the shipped description is relative to it. `reference` is a fact about the
capture session, stated by the tool that recorded it — never inferred by a reader. For a device
captured before anyone thought of measuring, it is also the one fact that cannot be recovered later:
the knob has to still be where it was. The reference position is
therefore flat by construction, so a player that ignores `tone` — or a lone `.namz` pulled out
of the pack, which carries no tone block — plays the reference tone, which is exactly what those
weights encode. Nothing is double-filtered and nothing lies.

**One form per knob, and there are four of them.** A tone knob is described by exactly ONE of:

* `positions[].db` — **the measured ladder**: a dB table per swept position. A dial or a switch.
* `sections` — **bands travelling on rotation**: the same knob as parametric filters whose gain runs
  with the dial. A DIAL only; the travel law below is stated in degrees, and a switch has none.
* `positions[].sections` — **a filter stated whole at each position**. A SWITCH only. Its positions are
  named, ordered, and have nothing in between them, so there is no rotation for a gain to travel on:
  each position says what it IS, and nothing is computed between two of them.
* `positions[].hz` — **a knob that moves another knob's band**: a frequency per position and no sound of
  its own. A dial or a switch.

Exactly one of the two keys `positions` / `sections` is present. A knob carrying both is not a knob with
a spare description, it is an **invalid manifest**: a reader refuses the whole file, not the knob,
because whichever form one reader picked another could pick the other, and the same pack would be
two products. `positions` stays in the format permanently; `sections` is the second form, not a
replacement. A reader with a manifest-less fallback then takes that fallback — the pack loads as the
headers alone describe it, with no tone knobs — exactly as it does for a `schema` it does not speak;
there is no third behaviour. A knob carrying NEITHER key, or `sections` without a `sweep`, is not an
invalid manifest: that knob is skipped and the reference position plays.

| field | meaning |
|---|---|
| `name` / `sweep` | the control and how far it turns, degrees — **`sweep` absent or 0 means a SWITCH**: a player draws a toggle over `positions` and does NOT interpolate between them, because a switch has nothing in between. With a sweep it is a dial: draw a knob and interpolate. A switch ships `positions`, with `db`, `sections` or `hz` on each of them; knob-level `sections` needs a sweep, because its travel law is stated in rotation. **A switch has no `norm`** — it has an order and no angle — and a position is addressed by its `value`, never by its place in the array, so a knob that loses a position loses that position and nothing else |
| `placement` | where the filter goes. `post` — after the whole stage, including after a `blend` mix. `wet` — inside the wet path, before the mix, which is where a tone stack in a blended pedal physically sits: applying such a curve `post` would filter the clean signal the blend exists to keep clean. An unknown value: SKIP the control rather than guess. A frequency knob (`positions[].hz`) has no filter to place and carries none: its band sits where the band's own knob puts it |
| `reference` | the position every file was captured at (the description is relative to it). With `positions` it MUST appear in `positions[]`, and its `db[]` MUST be all zeros — that is what "flat by construction" means, and a reader may assert it. A non-zero reference row is a broken pack: the curve would be applied on top of a model that already contains it. With `sections` it is the position where every band's gain is zero — the hinge of the travel law below |
| `default` | where a player starts the knob. Absent: start at `reference` |
| `operating_point` | the capture axes held while sweeping — provenance, and the honest limit of the measurement |
| `grid` | curves only — `positions[].db`: the log-spaced frequency grid they are sampled on. A knob that ships no curve (bands, either form, or a frequency knob) carries no grid. **Endpoints inclusive**: `f[i] = f_lo · (f_hi/f_lo)^(i / (points − 1))`, so `f[0]` is exactly `f_lo` and `f[points − 1]` exactly `f_hi`. The band-centre convention some analysers use would move the top of an 8-point 20 Hz – 20 kHz grid from 20 kHz to 13 kHz, and every reading with it |
| `trusted` | the band where the curve was shown to BE a filter, over what drive range, and how many levels showed it: `lo_hz`, `hi_hz`, `span_db`, `levels` — and the SAME band as grid indices, `lo_index`, `hi_index`, which are the authority: the hold rule is applied on the grid, and two readers rounding Hz to an index differently pick neighbouring points, decibels apart on a 30 dB tilt, so the producer says which points it meant. Outside the band, **hold the curve at the value it has at the nearest band edge** — that is the instruction, and it is one instruction, not two. Absent, or `levels` 1: nothing was tested, trust the whole grid. `hi_hz <= lo_hz` — and `lo_index > hi_index`, the same statement in indices — with `levels` 2 or more: tested and **failed everywhere** — do not apply this curve at all. `span_db` is the drive range actually swept (loudest weighed rung minus quietest), and `levels` is the FEWEST any single position was weighed at. With knob-level `sections` it is provenance — where the ladder the bands were fitted to held — and nothing to apply: a band is one filter, played whole. **Never present on a `positions[].sections` or a `positions[].hz` knob**: it is stated in grid indices, and a reader meeting a collapsed band ("tested, a filter nowhere") on a knob that has no grid reads it as a verdict on bands it should have played |
| `positions[]` | per position, in the control's own order: its curve (`db`) or its bands (`sections`) — see below |
| `sections[]` | the knob as bands, applied in series — see below. Magnitude-only filters, so their order is immaterial |

#### `positions` — the measured ladder

Each position carries the **measurement itself**, and nothing else:

| field | meaning |
|---|---|
| `value` | the position: degrees on a dial (`"0"`…`"300"`), the word on the panel on a switch (`"Sharp"`). It is the position's IDENTITY — everything that addresses a position addresses it by this |
| `norm` | 0…1 across the sweep. **A DIAL ONLY, and REQUIRED there**; a switch has an order and no angle, and must not carry it. Until schema 4 a switch shipped evenly spaced steps here — a number nobody measured, in a field that promises a measurement. Absent and `0.0` cannot be told apart in a reader's number, so this is not a convention but a rule: `norm` present without a `sweep`, or absent with one, is an invalid manifest |
| `db[]` | *(the curve form)* the measured response relative to `reference`, dB per `grid` point. **`20·log10` of an amplitude ratio**, never a power dB — a reader using `10^(db/10)` halves every deviation and nothing complains. Length MUST equal `grid.points`; a position where it does not is unusable and must be skipped, not truncated or padded. REQUIRED |
| `level_db` | *(the curve form)* that position's broadband level over the trusted band, dB. Derivable from `db[]`, and shipped anyway so that every player agrees what "the same loudness" means instead of each choosing its own band and weighting |
| `sections[]` | *(the switch's band form)* the filter this position IS — see below. Exclusive with `db`: a position carrying both is two descriptions of one thing, and the manifest is refused, exactly as for a knob carrying both forms |
| `hz` / `q` | *(the frequency form)* where the band this knob moves sits at this position — see `positions[].hz` below. Exclusive with `db` and `sections`, on the same terms |

A curve, not a filter. There are no bands, no Q, no coefficients here — just a magnitude table, in
physical units, on a stated frequency grid. Baked coefficients would be wrong at every sample rate
but the one they were designed at; a table is right at all of them. The player designs whatever it
likes from it — a minimum-phase FIR folded into the cabinet convolution it is already running costs
nothing at runtime.

An earlier draft of this format also shipped a fitted 1-pole corner (`lp1_hz`, `gain_db`) with a
`residual_db` to say how far the fit lay from the truth, so that a simple player could skip the
curve. Measuring real pedals killed it. A Big Muff tone control is a bass-cut/treble-boost blend —
about 30 dB of tilt across the band — and the best 1-pole fit missed it by 15 to 65 dB. A player
written against those numbers does not sound approximately right; it sounds like a different pedal.
The fields were removed rather than deprecated, because the failure was silent: everything parsed,
nothing complained, and the result was confidently wrong.

**Why a band at all.** Each position is swept several times, 6 dB apart. A filter does not care how
hard it is driven, so its curve comes back the same at every level; harmonic products and hiss do not.
Measured on a Big Muff tone control, the curves agreed within 0.8 dB up to 9 kHz and then disagreed by
4-8 dB by 12 kHz — so the +16 dB of treble that control really has is worth shipping, and the +24 dB
the same measurement claimed at 16 kHz is not. The curve is shipped whole either way; the band says
where to stop believing it. Nothing is cut, because a measurement that was thrown away cannot be
reconsidered when somebody understands it better.

**And why the band comes with a drive range.** Push the same pedal 40 dB below its capture level and it
stops distorting at all — at which point its bass really is a different filter, and no amount of
averaging will make the two agree. That is the pedal telling the truth, not the measurement failing, so
it cannot be rejected as noise. `span_db` is therefore a promise with its terms attached: this curve
held over this much drive range. Verified over 24 dB on the same Big Muff, its tone control is a filter
from 60 Hz up; measured over 42 dB, only from 700 Hz. Both numbers are correct about different
questions, and the one a player needs is the first.

**Interpolating — a DIAL only.** Between positions, per grid point, **in dB, against `norm`** — not
against the array index, which is the same thing only when the swept positions happen to be evenly
spaced. A dial's `positions[]` is sorted ascending by `norm`, so a reader may index and interpolate with
no search and no sort. **A SWITCH is not interpolated at all**: the player takes the position whose
`value` matches, and a value no position declares is not a knob setting — a player must refuse it and
keep the setting it had, rather than fall back to something that sounds like the reference. Outside
the outermost `norm`, **hold** the nearest swept curve; do not extrapolate, which on a 30 dB tilt invents
decibels. Between grid points, interpolate in **log frequency**.

**When two fields could disagree on a dial**, `norm` wins. It is derivable from `value` and `sweep`, and
a reader that recomputes it will interpolate differently from one that reads it; the shipped number is
the authority. On a SWITCH there is no second field to disagree: `value` is the whole address.

**A control's default position is `default`** when stated, and `reference` otherwise. A player must not
invent one: two readers starting the same pack at different knob positions make it two products. On a
SWITCH, `default` must name one of the positions — a pack cannot open on a setting the knob does not
have — and a reader that finds otherwise drops the `default` and opens at the reference. A dial keeps
whatever angle it states: any point of its travel is playable, swept or not.

**`reference` must appear in `positions[]`**, in every form. Everything the knob says is stated against
it, and without that row a player has nothing to make flat — it would interpolate between neighbours
exactly where the knob must be silent, and the models already carry that tone, so it would land twice.
A knob whose reference is missing (never named, or named and its row unusable) is DROPPED, and the
captured position plays. **A knob with one position is dropped too**: its only position must be the
reference, the reference is flat by construction, so the control cannot change a sound.

**What a host persists.** A DAW parameter is a continuous float, and a switch is not; a plugin maps one
onto the other while a hand is on it. What it must SAVE is the position's `value`, never the float: a
pack rebuilt with one position fewer would otherwise reopen an old session on a different setting, and
nothing would say so.

Only controls AFTER the non-linearity qualify: a knob before the clipper changes the drive into it, so its
effect is not linear and subtracting it is a lie.

#### `sections` — the knob as bands

The same knob, written as what it turned out to be: one or more parametric bands whose KIND is fixed
and whose GAIN moves with the dial. The kind — which shelf or bell, where it sits, how steep — is where
the circuit's character lives, and the gain is just a number, so a player can turn the number further
than the pedal ever went and still be turning that pedal's knob. A ladder has no handle for that. Where
the hardware's corner or Q also drifts along the travel, `hz_at` / `q_at` say so, and the law below
moves them with the same `t` that moves the gain.

```json
{ "type": "low_shelf" | "bell" | "high_shelf" | "tilt",
  "hz": 197, "q": 0.76, "range_db": [-8.9, 1.2] }
```

Optional keys: `pivot` (only on a `tilt`), `hz_at`: `[at the minus stop, at the plus stop]`, `q_at`:
the same for Q, `hz_by`: another knob that sets the frequency instead. A key that is absent means the
quantity does not move.

| field | meaning |
|---|---|
| `type` | the band's shape: `low_shelf`, `bell`, `high_shelf`, `tilt`. A `type` a reader does not know makes the **knob** unusable — skip the knob, do not play the bands you do know: the reference position is what every tone knob promises to be, a subset of its bands is a different pedal |
| `hz` / `q` | the band's centre (`bell`) or corner (shelves, `tilt`) and its Q **at `reference`**, where its gain is zero |
| `range_db` | the band's gain at the **minus stop** and at the **plus stop**, in that order, **SIGNED** — a cut is negative. The minus stop is the dial's 0, the plus stop is `sweep`: the order is by POSITION, never by sign, so a band that boosts at 0° carries a positive first entry. (The capture side handles the same pair as two magnitudes ordered by sign — most-cut, most-boost — so the pair may arrive REVERSED as well as unsigned; the pack does neither: `[-8.9, 1.2]` reads as 8.9 dB of cut at 0° and 1.2 dB of boost at full.) On a stop the reference sits on, that stop's entry is 0 — the law leaves it nothing else to be |
| `pivot` | `tilt` only: where the seesaw hinges, 0…1 — see below. Absent on a tilt: 0.5, the symmetric seesaw. Stored as written; a value outside 0…1 is clamped when applied, as the formula says. On any other `type` it is not read — a reader does not look at it, so it cannot spoil the band |
| `hz_at` / `q_at` | what `hz` / `q` become at the two stops, `[minus, plus]` — the same order as `range_db`, by position. Both entries strictly positive: a 0 is not a frequency, and a pair a reader cannot read makes the knob unusable (skip the knob). A side where the knob has no travel — `reference` IS that stop — carries the reference value itself. Absent: the quantity is one frozen value across the whole travel |
| `hz_by` | the `name` of the knob that sets this band's frequency — and its Q, if that knob states one — instead of this knob's travel: a FREQ under a MID. See `positions[].hz` below. Absent: this knob owns the frequency, as above |

**Computing a band.** RBJ cookbook, at the player's sample rate `fs`, with `hz`, `q` and `gain_db` the
band's values AT THE CURRENT POSITION — all three come from the travel law below, not just the gain:

```
A     = 10^(gain_db / 40)
w0    = 2π · hz / fs
cw    = cos(w0),  sw = sin(w0)
alpha = sw / (2 · q)

bell:
  b0 = 1 + alpha·A      b1 = -2·cw      b2 = 1 - alpha·A
  a0 = 1 + alpha/A      a1 = -2·cw      a2 = 1 - alpha/A

low_shelf:   s = 2·sqrt(A)·alpha
  b0 =     A·((A+1) - (A-1)·cw + s)
  b1 = 2·A·((A-1) - (A+1)·cw)
  b2 =     A·((A+1) - (A-1)·cw - s)
  a0 =       (A+1) + (A-1)·cw + s
  a1 = -2·  ((A-1) + (A+1)·cw)
  a2 =       (A+1) + (A-1)·cw - s

high_shelf and tilt:   s = 2·sqrt(A)·alpha
  b0 =      A·((A+1) + (A-1)·cw + s)
  b1 = -2·A·((A-1) + (A+1)·cw)
  b2 =      A·((A+1) + (A-1)·cw - s)
  a0 =        (A+1) - (A-1)·cw + s
  a1 =  2·   ((A-1) - (A+1)·cw)
  a2 =        (A+1) - (A-1)·cw - s

The coefficients are normalised by a0.  hz is clamped to [10, fs·0.49], q to [0.05, 20].
```

**`tilt`** is the same coefficients as `high_shelf`, plus a factor on the numerator:

```
trim = 10^(-clamp(pivot, 0, 1) · gain_db / 20)
b0, b1, b2 are multiplied by trim
```

So a tilt is a high shelf with the floor taken out from under it: the shelf still lifts the top by
`gain_db`, and the trim drops everything by `pivot · gain_db`, which leaves the two arms straddling
the hinge — the top arm ends at `+gain_db · (1 − pivot)`, the bottom arm at `−gain_db · pivot`. Half
is the seesaw everyone pictures; the hardware rarely agrees, which is why the value is fitted and
shipped rather than assumed.

**How a band moves with the dial.**

- `hz` and `q` — the values at the position the takes were shot (`reference`), where the band's gain
  is zero.
- `hz_at`, `q_at` — the values at the two stops of the knob.
- Between the stops: the frequency is interpolated geometrically, Q linearly, the gain linearly in dB.
- `range_db` — the gain at the minus stop and at the plus stop.

The gain is zero AT `reference`, and it runs linearly in dB from there to each stop — two straight
segments meeting at zero, **not** one straight line drawn through the two stops (those coincide only
when the reference is itself a stop). Write the position as `t`: the knob's rotation normalised so
that `reference` is 0, the minus stop is −1 and the plus stop is +1. `reference` is a bare integer of
degrees, 0…`sweep`, like every dial position; one that is not a position on that dial makes the knob
unusable (skip the knob). With `norm = value / sweep` and `norm_ref = reference / sweep`:

```
t = 0                                      at  norm = norm_ref     (the reference: flat)
t = (norm − norm_ref) / norm_ref           for norm < norm_ref     (−1 at the minus stop)
t = (norm − norm_ref) / (1 − norm_ref)     for norm > norm_ref     (+1 at the plus stop)

gain_db(t) = −t · range_db[0]              for t <  0
gain_db(t) =  t · range_db[1]              for t >= 0

side       = [0] for t < 0, [1] for t >= 0
hz(t)      = hz · (hz_at[side] / hz) ^ |t|          (geometric; hz(0) = hz)
q(t)       = q  + (q_at[side]  − q) · |t|           (linear;    q(0) = q)
```

The frequency and the Q split at the reference exactly as the gain does: which stop they head for is
decided by the SIGN of `t`, how far along by `|t|`. A reader that ran the frequency as one straight
line through both stops would put a different corner under the same knob position.

`t` is a function of POSITION, and of nothing else — it is never derived from a band's gain. The two
readings agree only because the gain is declared linear in position above; a reader that recovered
`t` from `gain_db` instead would draw the right curve for a fitted band and a different one for a
hand-authored band, and nothing would complain. A stop the reference sits on has no positions on its
side, so its divisor is never used: with `reference` at the plus stop, `norm > norm_ref` never happens,
and the reference itself is the `t = 0` line — which is why that line comes first.

Between the stops nothing is held or extrapolated: the knob cannot go past a stop, and every position
inside is defined. A band is applied whole; `trusted` is provenance here, not an instruction.

A band that carries `hz_by` takes its frequency from that knob's position instead of from `t` — and its Q
too, where that knob states one. Its gain still runs on `t`, exactly as above.

#### `positions[].sections` — a knob that clicks, stating each filter

A switch's positions are named, ordered, and have **nothing in between them**. There is no rotation for
a band's gain to travel on, so a switch does not get a travel law: each position states the filter it
IS, against `reference`.

```json
{ "name": "Edge", "placement": "post", "reference": "Sharp",
  "positions": [
    { "value": "Sharp",  "sections": [] },
    { "value": "Smooth", "sections": [ { "type": "high_shelf", "hz": 10000.0, "q": 0.80, "gain_db": -9.0 } ] }
  ] }
```

| field | meaning |
|---|---|
| `type` / `hz` / `q` / `pivot` | exactly as in `sections` above — the same four shapes, the same RBJ formulas, and `pivot` on a tilt only |
| `gain_db` | what this band is worth HERE, signed, against `reference`. One number, not a pair: there is no stop to travel to |

**The anchor states nothing, and says so.** `reference` ships `"sections": []` — spelled out, not
omitted. At that position the models already are the sound, and an empty list is the statement "flat
here"; a missing key would be an omission, and the two must not look alike.

**Forbidden here:** `range_db`, `hz_at`, `q_at`, `hz_by`. They describe a journey between two stops, and
this knob has no stops — nor one band for another knob to move: each position is a filter of its own. A
manifest carrying one is refused rather than read with the key ignored — a reader that ignored it would
play a fixed band where the producer thought it wrote a travelling one.

**A band this reader cannot build drops the KNOB, never one band** — an unknown `type`, no frequency, no
Q, a gain that is not a number. Playing the bands understood and dropping the rest is a different pedal;
playing the reference is what dropping a tone knob has always meant.

**Switching is not a parameter change.** Between two positions the CHAIN may change — a different count
of bands, a different shape — so a player must not ramp band *k*'s coefficients from one position's to
the next's: that walks through filters neither position contains, and an appearing band clicks. Build
the new cascade beside the old one and cross-fade the OUTPUT over a few milliseconds.

#### `positions[].hz` — a knob that moves another knob's band

Some panels split one band across two controls. On a bass preamp the MID knob is a bell's gain, and the
FREQ knob under it moves that bell's centre and does nothing else — the bell just moves. So FREQ has no
sound of its own: with MID at its reference the bell is flat wherever FREQ points. What it ships is where
the band sits, per position, and the band names it:

```json
"tone": [
  { "name": "mid", "sweep": 300, "placement": "post", "reference": "150",
    "sections": [
      { "type": "bell", "hz": 800, "q": 0.9, "range_db": [-15.0, 15.0], "hz_by": "freq" }
    ] },
  { "name": "freq", "sweep": 300, "reference": "150",
    "positions": [
      { "value": "0",   "norm": 0.0, "hz": 220 },
      { "value": "150", "norm": 0.5, "hz": 800 },
      { "value": "300", "norm": 1.0, "hz": 3000 }
    ] }
]
```

A FREQ that clicks — a five-way switch at 220, 450, 800, 1600 and 3000 Hz — is the same list with no
`sweep` and no `norm`, its `value`s the words on the panel.

| field | meaning |
|---|---|
| `hz_by` | on a band of a dial's `sections`: the `name` of the knob, in the same stage's `tone`, that sets this band's frequency — and its Q, where that knob states one. One string, so a band has at most one frequency owner |
| `positions[].hz` | the band's frequency at this position, Hz: a number, strictly positive. Required on every position |
| `positions[].q` | the band's Q at this position, strictly positive. On EVERY position or on none: none means the Q stays the band's own (its `q`, travelling on its `q_at` if it has one); some is a knob that says Q at one position and is silent at the next, and is unusable |

**The reference is where the band was measured.** FREQ's `reference` is the position every file was
captured at, as for every tone knob, and MID's bands were measured with FREQ there. So the band's `hz` and
FREQ's `hz` at its reference are one frequency stated twice, and they MUST be equal — and the band's `q`
and FREQ's reference `q` likewise, where FREQ states Q. Two statements of one frequency that disagree are
two products, and the manifest is refused. The invariant needs nothing else: at MID's reference the band
is flat wherever FREQ points, so the files play their own tone at every FREQ position.

**How the band moves.** On a DIAL, between two neighbouring positions `a` and `b`, against `norm`:

```
u      = (norm − norm_a) / (norm_b − norm_a)
hz     = hz_a · (hz_b / hz_a) ^ u           (geometric)
q      = q_a  + (q_b − q_a) · u             (linear)
```

and outside the outermost `norm`, hold the nearest position. Two positions at the stops move the
frequency log-evenly across the whole sweep; more positions say where the hardware does otherwise. A
dial's `norm`s are inside 0…1 and strictly ascending, or there is nothing to interpolate between. A
SWITCH is not interpolated: the position whose `value` matches, nothing in between, and a change of
position is a change of filter — cross-fade the output, as for `positions[].sections`. The band that
plays is then: its `type` from itself, its gain from its own knob's travel law, its frequency from FREQ,
and its Q from FREQ where FREQ states it, from its own knob otherwise.

**One owner per quantity.** A band that names a frequency knob carries no `hz_at` — its own knob would be
a second owner of the same frequency — and, where that knob states Q, no `q_at`. A frequency knob moves
exactly ONE band: named by none, it is a knob that moves nothing; named by two, one dial would be
pretending to be two. A frequency knob has no band of its own, so nothing can name it from inside it and
a link is one step long — there is no chain to close into a cycle. A knob that is both — bands and
frequencies, or a frequency and a curve on one position — is two forms, refused like any other.

**Refused** — the manifest is invalid, for the reason every refusal in this format has: a reader that
let it through would have to guess, and two readers would guess differently:

* `hz_by` that is not a string, or names anything but exactly ONE frequency knob of the same stage — no
  knob, a knob of another form (the band's own knob included), two knobs by that name;
* `hz_by` on a band of `positions[].sections`;
* a band naming a frequency knob that also carries `hz_at`, or `q_at` where that knob states Q;
* two bands naming one frequency knob, or a frequency knob no band names;
* `hz` on a position beside `db` or `sections`, or a knob whose positions disagree about their form;
* the band's `hz` — or `q`, where FREQ states Q — not equal to FREQ's at its reference.

**Dropped** — one knob cannot be played, and every reader applying these rules sees the same thing: a
position whose `hz` (or `q`) is not a strictly positive number, `q` on some positions and not others, a
dial's `norm`s out of 0…1 or not ascending, and every rule a `positions` knob already has (its
reference missing, one position). A frequency knob that is dropped plays its reference: the band stays at
its own `hz`, and its Q follows its own knob. A band's knob that is dropped takes its frequency knob with it — there is nothing
left for it to move. The equality above is checked between the two knobs that remain; a frequency knob
that did not survive has no reference row to compare.

### `blend` — a dry/wet mix knob

Neither of the other two roles fits. A blend is not a filter, so `tone` cannot describe it. And it
must not become a captured axis: the dry signal IS the DI the model is already being fed, so eleven
blend positions as an axis would be eleven copies of one model plus arithmetic the player does for free.
A blend costs **zero** extra takes.

What the pack ships is the mix itself:

```
out = polarity · dry_gain(pos) · (DI * dry) + wet_gain(pos) · model(DI)
```

```json
"blend": [
  { "name": "blend", "sweep": 300, "reference": "300", "dry_end": "0",
    "law": "linear", "polarity": -1,
    "grid": { "f_lo": 20.0, "f_hi": 20000.0, "points": 4 },
    "trusted": { "lo_hz": 40.0, "hi_hz": 12000.0, "span_db": 24.0, "levels": 5 },
    "dry": [0.0, -1.0, -9.0, -24.0], "dry_level_db": -9.0,
    "positions": [
      { "value": "0",   "norm": 0.0, "dry_db": 0.0,    "wet_db": -120.0 },
      { "value": "150", "norm": 0.5, "dry_db": -6.0,   "wet_db": -6.0 },
      { "value": "300", "norm": 1.0, "dry_db": -120.0, "wet_db": 0.0 }
    ] }
]
```

| field | meaning |
|---|---|
| `name` / `sweep` | the knob and its rotation; `sweep` absent or 0 means a switch. Same rule as `tone`: `norm` on a position exists exactly where `sweep` does, and a manifest that says otherwise is refused |
| `reference` | the full-WET end — where every model of the stage was captured. Same invariant as `tone`: ignore `blend` and you play full wet, which is exactly what the weights encode |
| `dry_end` | the full-DRY end, where `dry` was measured. At that position the output is the dry path ALONE and therefore linear, which is the whole reason a blend can be characterised from outside the box |
| `dry[]` | the dry path's response SHAPE, dB per `grid` point, with its own broadband level removed. On a bass pedal this is almost never flat — keeping a clean low end out of the distortion is what the circuit is for |
| `dry_level_db` | the level that was removed: the dry path's broadband gain, dB, on the same scale as the model's own output. REQUIRED, and not derivable from anything else in the pack — a reader cannot re-measure it, because the pedal is not in their hands. Omit it and a dry path with 9 dB of insertion loss renders 9 dB too loud at every position that is not an end |
| `trusted` | as in `tone`: where `dry` was shown to be a filter, over what drive range |
| `polarity` | the sign of the dry branch **against the model's output**, `+1` or `-1`. Defined that way and not "the box inverts one path", because a neural model is trained on the waveform and therefore already contains the wet path's own inversion: stating it against the hardware would have producer and consumer applying opposite signs. Measured by deconvolving a sweep at each end and comparing the sign of the impulse response's leading edge — the same recordings the rest of the block comes from |
| `law` | how the gains were derived (`linear`, `equal_power`, …) — PROVENANCE only, never something a reader implements |
| `positions[]` | per position: `dry_db` and `wet_db`, the two gains. On a SWITCH **both are REQUIRED at every position**, or the knob is dropped: between two detents there is nothing to derive from, and a reader's defaults (silent dry, unity wet) are the reference itself — so a middle position that said nothing would play as full wet, a hundred per cent wrong at a setting the panel offers. A dial may lean on its `law`; a switch has no law to lean on |
| `gains_measured` | whether `dry_db`/`wet_db` were MEASURED or derived from `law`. A derived pair assumes the pot is an amplitude-linear crossfade and a real one often is not, and a reader cannot tell from the numbers. `false` means treat them as an estimate |
| `positions[]` | `value`, `norm` (**rotation**, exactly as for `tone`, ascending), and the two gains `dry_db` / `wet_db`. `-120` means **exactly silent**, not 10⁻⁶: a literal conversion leaks both branches at the ends |

**Interpolating the gains** between positions is done in **amplitude**, not in dB. Between `wet_db: -6`
and `wet_db: -120` a dB interpolation puts the halfway point at −63 dB — inaudible — where the amplitude
one puts it at −12; both readings are otherwise permitted by the same text, and they are 50 dB apart at
the same knob position.

**Phase and time.** The dry branch is summed with `model(DI)` in **parallel**, so how a reader realises
`dry[]` decides the result, not just its tone. Build it **minimum phase**, and keep the two branches
sample-aligned: a linear-phase FIR long enough to shape 80 Hz carries about 10 ms of bulk delay, and a
blend with 10 ms between its branches is a flanger. For a series `tone` filter the choice is a
nuance; here it is the whole sound.

**Order within a stage.** `tone` controls compose in any order among themselves — magnitude-only
linear filters commute. Against a `blend` they do not: apply every `placement: "wet"` filter to the
model's output first, then the mix, then every `placement: "post"` filter to the result.

A reader that does not implement `blend` skips it and plays the reference, exactly as with `tone`.

**Control values are JSON strings**, always — `"300"`, not `300`. A degree is written as a bare integer
inside that string, which is a different statement from being a JSON number, and a reader is entitled to
accept only strings. A producer emitting numbers gets every position silently discarded.

**`schema` is a duty, not a decoration.** A reader MUST refuse a manifest whose `schema` is higher than
the one it implements. Additive keys never bump it, so a bump means something a reader of that vintage
would get wrong — and loading it anyway, recognising what it knows and dropping the rest, is precisely
the silent-and-confident failure the format's removal policy exists to prevent.

**`family_id`** groups packs that are one physical box cut into several — a device captured as three
packs plus its boost is still one pedal, and a player may offer them together. It is stamped into every
`.namz` header as well as the manifest, so a file separated from its pack can still be recognised as
part of the family. Nothing in this format requires a reader to act on it; it is there so that the fact
survives, because it cannot be reconstructed later.

The current `schema` is **5** (4 → 5: a knob learned to move ANOTHER knob's band — `positions[].hz`,
named by a band's `hz_by`. A schema-4 reader meets the frequency knob, finds no `grid`, never reads its
positions and drops it as a knob carrying neither form, and it plays the band at one frequency: the
author declared a knob and the pack would play without it, silently — the failure 3 → 4 was raised for.
3 → 4: a knob that CLICKS learned to ship bands — `positions[].sections`
— and two rules came with it: `norm` exists only where rotation exists, and a switch blend states both
gains at every position instead of leaning on a law. A schema-3 reader meets such a knob, finds no
`grid`, never reads its positions and drops it as a knob carrying neither form: the author declared
bands and the pack would play the reference, silently. 2 → 3: `measured` became `tone` and grew
`sections`.) Had the number
stayed at 2, a schema-2 reader would have found no `measured` key and played every tone knob flat,
silently — the exact failure the number exists to catch; at 3 it refuses instead. `measured` is not
read: no pack carrying it was ever published. `schema` is a JSON integer; a reader refuses a manifest
whose `schema` is anything else, because a number it cannot judge is not a number it may assume.

Additive keys never bump `schema`; a bump is reserved for an incompatible change (avoided by
design). The reference reader/selector is `namz::rig`.

## Versioning

`formatVersion`, `codec`, and `dtype` are single bytes with reserved values, so the format can grow
without breaking old readers: the `float16` dtype (1) arrived this way in 4.4.0 without a new
`formatVersion`, and a future `zstd` codec (2) would do the same. A reader that doesn't understand a byte
value refuses the file cleanly rather than mis-decoding it — a 4.3.x reader refuses a `.nam16z`.
