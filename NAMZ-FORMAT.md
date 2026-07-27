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
  rig.json                         the manifest — chain + controls + measured + file index + EQ hints
  m01.namz                         models (codec=store), one per knob/switch combination
  ...
```

**`rig.json`** is the pack's source of truth; each `.namz` still carries its own
`settings.*`/`controls`/`rig_id`/`slot` header, so a single file pulled out of the pack still knows
what it is (a player with no manifest falls back to scanning `.namz` + reading those headers).

```json
{
  "format": "orbitrig", "schema": 1,
  "rig_id": "dc-ts9", "name": "Golden Drive", "modeled_by": "Darwin's Cat",
  "chain": [
    { "kind": "nam", "slot": "pedal",
      "gear": { "make": "Darwin's Cat", "model": "Golden Drive", "type": "pedal" },
      "tone_type": "crunch",
      "controls": [
        { "name": "gain", "role": "gain", "sweep": 300, "values": ["0","150","300"] }
      ],
      "measured": [
        { "name": "tone", "sweep": 300, "placement": "post", "reference": "300",
          "operating_point": { "gain": "150" },
          "grid": { "f_lo": 20.0, "f_hi": 20000.0, "points": 8 },
          "trusted": { "lo_hz": 60.0, "hi_hz": 4600.0, "span_db": 24.0, "levels": 5 },
          "positions": [
            { "value": "0",   "norm": 0.0, "db": [-0.2, -0.2, -0.4, -0.9, -3.2, -9.4, -16.8, -24.1] },
            { "value": "300", "norm": 1.0, "db": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0] }
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
- **`files`** — the authoritative list for a `nam` stage: each entry is a model + its `settings`.
  (A manifest-less folder is read by scanning `*.namz` instead.)
- **`measured`** — knobs that are NOT model axes: see below.
- **`blend`** — dry/wet mix knobs, the third kind of control: see below.
- **`eq` stage** — the tone stack is ALWAYS software; this stage is optional author guidance for the
  player's EQ, never captured audio. Fields (all optional): `model` (e.g. `fmv`), `defaults` (a
  starting value for any EQ knob), `hidden` (knobs to hide, e.g. `["hpf","lpf"]`), `tone_only` (a
  single simplified "TONE" knob instead of the full EQ), `show_curve` (draw the EQ response or not).

### `measured` — linear knobs the capture never turned into axes

A tone control after the clipper is **linear**: turning it changes the frequency response, not the
distortion. Capturing it as a matrix axis would multiply the take count for nothing — a 6-position
tone knob turns 21 takes into 126. So the capture tool measures it instead: one short sweep per
position at a fixed operating point, deconvolved against a reference position. The pack ships the
recovered response, the player applies it as DSP, and the knob it draws is CONTINUOUS — the whole
point of measuring rather than capturing.

Format invariant: **every file of the stage was captured with each measured control at its
`reference` position**, and the shipped responses are relative to it. `reference` is a fact about the
capture session, stated by the tool that recorded it — never inferred by a reader. For a device
captured before anyone thought of measuring, it is also the one fact that cannot be recovered later:
the knob has to still be where it was. The reference position is
therefore flat by construction, so a player that ignores `measured` — or a lone `.namz` pulled out
of the pack, which carries no measured block — plays the reference tone, which is exactly what those
weights encode. Nothing is double-filtered and nothing lies.

| field | meaning |
|---|---|
| `name` / `sweep` | the control and how far it turns, degrees — **`sweep` absent or 0 means a SWITCH**: a player draws a toggle over `positions` and does NOT interpolate between them, because a switch has nothing in between. With a sweep it is a dial: draw a knob and interpolate |
| `placement` | where the filter goes. `post` — after the whole stage, including after a `blend` mix. `wet` — inside the wet path, before the mix, which is where a tone stack in a blended pedal physically sits: applying such a curve `post` would filter the clean signal the blend exists to keep clean. An unknown value: SKIP the control rather than guess |
| `reference` | the position every file was captured at (curves are relative to it). It MUST appear in `positions[]`, and its `db[]` MUST be all zeros — that is what "flat by construction" means, and a reader may assert it. A non-zero reference row is a broken pack: the curve would be applied on top of a model that already contains it |
| `default` | where a player starts the knob. Absent: start at `reference` |
| `operating_point` | the capture axes held while sweeping — provenance, and the honest limit of the measurement |
| `grid` | the log-spaced frequency grid `db` is sampled on. **Endpoints inclusive**: `f[i] = f_lo · (f_hi/f_lo)^(i / (points − 1))`, so `f[0]` is exactly `f_lo` and `f[points − 1]` exactly `f_hi`. The band-centre convention some analysers use would move the top of an 8-point 20 Hz – 20 kHz grid from 20 kHz to 13 kHz, and every reading with it |
| `trusted` | the band where the curve was shown to BE a filter, over what drive range, and how many levels showed it: `lo_hz`, `hi_hz`, `span_db`, `levels`. Outside the band, **hold the curve at the value it has at the nearest band edge** — that is the instruction, and it is one instruction, not two. Absent, or `levels` 1: nothing was tested, trust the whole grid. `hi_hz <= lo_hz` with `levels` 2 or more: tested and **failed everywhere** — do not apply this curve at all. `span_db` is the drive range actually swept (loudest weighed rung minus quietest), and `levels` is the FEWEST any single position was weighed at |
| `positions[]` | per measured position, in dial order |

Each position carries the **measurement itself**, and nothing else:

| field | meaning |
|---|---|
| `value` / `norm` | the position, and the same as 0…1 — rotation for a dial, evenly spaced steps for a switch, so a widget maps onto `norm` without knowing which it has. Entries are in the control's own order |
| `db[]` | the measured response relative to `reference`, dB per `grid` point. **`20·log10` of an amplitude ratio**, never a power dB — a reader using `10^(db/10)` halves every deviation and nothing complains. Length MUST equal `grid.points`; a position where it does not is unusable and must be skipped, not truncated or padded. REQUIRED |
| `level_db` | that position's broadband level over the trusted band, dB. Derivable from `db[]`, and shipped anyway so that every player agrees what "the same loudness" means instead of each choosing its own band and weighting |

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

**Interpolating.** Between positions, per grid point, **in dB, against `norm`** — not against the array
index, which is the same thing only when the swept positions happen to be evenly spaced. `positions[]`
is sorted ascending by `norm`, so a reader may index and interpolate with no search and no sort. Outside
the outermost `norm`, **hold** the nearest swept curve; do not extrapolate, which on a 30 dB tilt invents
decibels. Between grid points, interpolate in **log frequency**.

**When two fields could disagree**, `norm` wins. It is derivable from `value` and `sweep` for a dial, and
a reader that recomputes it will interpolate differently from one that reads it; the shipped number is
the authority and `value` is the caption a human reads.

**A control's default position is `default`** when stated, and `reference` otherwise. A player must not
invent one: two readers starting the same pack at different knob positions make it two products.

Only controls AFTER the non-linearity qualify: a knob before the clipper changes the drive into it, so its
effect is not linear and subtracting it is a lie.

### `blend` — a dry/wet mix knob

Neither of the other two roles fits. A blend is not a filter, so `measured` cannot describe it. And it
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
| `name` / `sweep` | the knob and its rotation; `sweep` absent or 0 means a switch |
| `reference` | the full-WET end — where every model of the stage was captured. Same invariant as `measured`: ignore `blend` and you play full wet, which is exactly what the weights encode |
| `dry_end` | the full-DRY end, where `dry` was measured. At that position the output is the dry path ALONE and therefore linear, which is the whole reason a blend can be characterised from outside the box |
| `dry[]` | the dry path's response SHAPE, dB per `grid` point, with its own broadband level removed. On a bass pedal this is almost never flat — keeping a clean low end out of the distortion is what the circuit is for |
| `dry_level_db` | the level that was removed: the dry path's broadband gain, dB, on the same scale as the model's own output. REQUIRED, and not derivable from anything else in the pack — a reader cannot re-measure it, because the pedal is not in their hands. Omit it and a dry path with 9 dB of insertion loss renders 9 dB too loud at every position that is not an end |
| `trusted` | as in `measured`: where `dry` was shown to be a filter, over what drive range |
| `polarity` | the sign of the dry branch **against the model's output**, `+1` or `-1`. Defined that way and not "the box inverts one path", because a neural model is trained on the waveform and therefore already contains the wet path's own inversion: stating it against the hardware would have producer and consumer applying opposite signs. Measured by deconvolving a sweep at each end and comparing the sign of the impulse response's leading edge — the same recordings the rest of the block comes from |
| `law` | how the gains were derived (`linear`, `equal_power`, …) — PROVENANCE only, never something a reader implements |
| `gains_measured` | whether `dry_db`/`wet_db` were MEASURED or derived from `law`. A derived pair assumes the pot is an amplitude-linear crossfade and a real one often is not, and a reader cannot tell from the numbers. `false` means treat them as an estimate |
| `positions[]` | `value`, `norm` (**rotation**, exactly as for `measured`, ascending), and the two gains `dry_db` / `wet_db`. `-120` means **exactly silent**, not 10⁻⁶: a literal conversion leaks both branches at the ends |

**Interpolating the gains** between positions is done in **amplitude**, not in dB. Between `wet_db: -6`
and `wet_db: -120` a dB interpolation puts the halfway point at −63 dB — inaudible — where the amplitude
one puts it at −12; both readings are otherwise permitted by the same text, and they are 50 dB apart at
the same knob position.

**Phase and time.** The dry branch is summed with `model(DI)` in **parallel**, so how a reader realises
`dry[]` decides the result, not just its tone. Build it **minimum phase**, and keep the two branches
sample-aligned: a linear-phase FIR long enough to shape 80 Hz carries about 10 ms of bulk delay, and a
blend with 10 ms between its branches is a flanger. For a series `measured` filter the choice is a
nuance; here it is the whole sound.

**Order within a stage.** `measured` controls compose in any order among themselves — magnitude-only
linear filters commute. Against a `blend` they do not: apply every `placement: "wet"` filter to the
model's output first, then the mix, then every `placement: "post"` filter to the result.

A reader that does not implement `blend` skips it and plays the reference, exactly as with `measured`.

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

Additive keys never bump `schema`; a bump is reserved for an incompatible change (avoided by
design). The reference reader/selector is `namz::rig`.

## Versioning

`formatVersion`, `codec`, and `dtype` are single bytes with reserved values, so the format can grow
without breaking old readers: a future `zstd` codec (2) or `float16` dtype (1) would bump those bytes, and
a reader that doesn't understand them refuses cleanly rather than mis-decoding.
