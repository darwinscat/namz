// SPDX-License-Identifier: MIT
// namz_rig — the .namz DEVICE MODEL: turn a family of .nam/.namz files into a device with working
// controls, and answer "the user turned one knob — which file plays now?". The player-side twin of
// the capture-side conventions in NAMZ-FORMAT.md ("Capture-identity keys"): controls come from the
// `controls` spec + per-file `settings.*` metadata (read them cheaply with namz::readMeta), with a
// nothing else. A filename is a caption, never evidence: it is an id the caller echoes back, and a
// file with no metadata is a device with no knobs. (Earlier versions parsed names for colour/chN,
// `NNh` gain, `boost`, PP/SE — that grammar is gone. Knob positions are DEGREES of dial rotation,
// see detail::degrees, and they live in `settings.*`.)
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.
//
// Header-only, C++17, std-only (no nlohmann, no namz.h dependency — metadata arrives as an
// already-parsed map). Message-thread only; selection results are stable file ids the caller maps
// back to its own entries.
//
// Selection policy (generalized from OrbitCab's PreampSelector): pin the control the user just
// changed, keep every other control where it is; when that exact combination was never captured,
// pick the candidate matching the most remaining controls, breaking ties toward each control's
// default (mid-sweep gain · boost off · first value) and then capture order. The returned file's actual
// settings are written back so the UI always shows a combination that truly exists.

#ifndef NAMZ_RIG_H
#define NAMZ_RIG_H

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace namz::rig
{

enum class Role { Channel, Gain, Boost, Topology, Generic };

inline const char* roleToString (Role r)
{
    switch (r)
    {
        case Role::Channel:  return "channel";
        case Role::Gain:     return "gain";
        case Role::Boost:    return "boost";
        case Role::Topology: return "topology";
        case Role::Generic:  return "generic";
    }
    return "generic";
}

inline Role roleFromString (const std::string& s)
{
    if (s == "channel")  return Role::Channel;
    if (s == "gain")     return Role::Gain;
    if (s == "boost")    return Role::Boost;
    if (s == "topology") return Role::Topology;
    return Role::Generic;
}

// A control the player draws and the file index selects on. `values` are the CAPTURED positions —
// for a dial they are degrees of rotation ("0" … "300"), and `sweep` is how far that dial physically
// turns, so a player can place a knob without knowing the device.
struct Control
{
    std::string name;
    Role role = Role::Generic;
    std::vector<std::string> values;   // capture/dial order
    int sweep = 0;                     // dial rotation, degrees; 0 = not a dial (switch/selector)
};

using Settings = std::map<std::string, std::string>;

// ---------------------------------------------------------------------------------------------
// The `controls` spec string (NAMZ-FORMAT conventional key): "name:role=v1|v2|…; name:role=…"
// ---------------------------------------------------------------------------------------------

namespace detail
{
    inline std::string trim (const std::string& s)
    {
        std::size_t a = 0, b = s.size();
        while (a < b && std::isspace ((unsigned char) s[a])) ++a;
        while (b > a && std::isspace ((unsigned char) s[b - 1])) --b;
        return s.substr (a, b - a);
    }

    inline std::vector<std::string> split (const std::string& s, char sep)
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= s.size(); ++i)
            if (i == s.size() || s[i] == sep)
            {
                out.push_back (s.substr (start, i - start));
                start = i + 1;
            }
        return out;
    }

    inline std::string lower (std::string s)
    {
        std::transform (s.begin(), s.end(), s.begin(),
                        [] (unsigned char c) { return (char) std::tolower (c); });
        return s;
    }
} // namespace detail

inline std::string buildControlsSpec (const std::vector<Control>& controls)
{
    std::string out;
    for (const auto& c : controls)
    {
        if (! out.empty()) out += "; ";
        out += c.name;
        out += ':';
        out += roleToString (c.role);
        out += '=';
        for (std::size_t i = 0; i < c.values.size(); ++i)
        {
            if (i > 0) out += '|';
            out += c.values[i];
        }
    }
    return out;
}

inline std::vector<Control> parseControlsSpec (const std::string& spec)
{
    std::vector<Control> out;
    for (const auto& rawEntry : detail::split (spec, ';'))
    {
        const auto entry = detail::trim (rawEntry);
        if (entry.empty()) continue;
        const auto colon = entry.find (':');
        const auto eq    = entry.find ('=', colon == std::string::npos ? 0 : colon);
        if (colon == std::string::npos || eq == std::string::npos || eq < colon) continue;
        Control c;
        c.name = detail::trim (entry.substr (0, colon));
        c.role = roleFromString (detail::trim (entry.substr (colon + 1, eq - colon - 1)));
        for (const auto& v : detail::split (entry.substr (eq + 1), '|'))
            if (auto t = detail::trim (v); ! t.empty()) c.values.push_back (t);
        if (! c.name.empty() && ! c.values.empty()) out.push_back (std::move (c));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Value vocabulary
// ---------------------------------------------------------------------------------------------

namespace detail
{
    // A dial position is DEGREES of rotation as a bare integer: "0", "150", "300". (The clock
    // grammar this format shipped with — "07h".."17h" — is gone: degrees need no lookup table to
    // place a knob, and `Control::sweep` says how far the dial goes.)
    inline int degrees (const std::string& v)   // "150" → 150; not a bare integer → -1
    {
        const auto l = trim (v);
        if (l.empty() || l.size() > 6) return -1;
        for (char ch : l)
            if (! std::isdigit ((unsigned char) ch)) return -1;
        return std::stoi (l);
    }
    inline bool isFalsy (const std::string& l)
    {
        return l == "off" || l == "no" || l == "false" || l == "0" || l.empty();
    }
} // namespace detail

// One file going INTO the model: a stable id (path/basename — echoed back by selection), the
// basename for display only, and the metadata map that says what the file actually is.
struct FileMeta
{
    std::string id;
    std::string filenameBase;                    // no extension
    std::map<std::string, std::string> meta;    // namz::readMeta output (or empty)
};

struct FileEntry
{
    std::string id;
    Settings settings;
};

struct Device
{
    std::string family;                          // display name
    std::string rigId;                           // stable id when stamped ("" otherwise)
    std::string slot;                            // pedal | preamp | amp | poweramp | rig | ""
    std::vector<Control> controls;
    std::vector<FileEntry> files;

    const FileEntry* find (const Settings& s) const
    {
        for (const auto& f : files)
            if (f.settings == s) return &f;
        return nullptr;
    }
};

// Per-control default: the gain dial's middle, falsy boost, first value otherwise. "Middle" is half
// the declared `sweep` when the dial states one, else the midpoint of the captured degrees — the
// generalization of the clock era's "noon", which only ever meant the same thing.
inline std::string defaultValue (const Control& c)
{
    if (c.values.empty()) return {};
    if (c.role == Role::Gain)
    {
        int lo = 1 << 30, hi = -1;
        for (const auto& v : c.values)
            if (const int d = detail::degrees (v); d >= 0) { lo = std::min (lo, d); hi = std::max (hi, d); }
        if (hi < 0) return c.values.front();               // not a degree dial — capture order decides
        const int mid = c.sweep > 0 ? c.sweep / 2 : (lo + hi) / 2;
        std::string best = c.values.front();
        int bestD = 1 << 30;
        for (const auto& v : c.values)
            if (const int d = detail::degrees (v); d >= 0 && std::abs (d - mid) < bestD)
            {
                bestD = std::abs (d - mid);
                best = v;
            }
        return best;
    }
    if (c.role == Role::Boost)
        for (const auto& v : c.values)
            if (detail::isFalsy (detail::lower (detail::trim (v)))) return v;
    return c.values.front();
}

inline Settings defaultSettings (const Device& d)
{
    Settings s;
    for (const auto& c : d.controls) s[c.name] = defaultValue (c);
    return s;
}

// Build devices from a set of files. Grouping key: `rig_id` when stamped, else `gear_model`, else
// the filename. Controls come from the file's `controls` spec VERBATIM (first spec seen wins) — a
// filename is never parsed for them. A file without that key is simply its own device with no
// knobs: no metadata, no device model. (The format shipped with a filename-token grammar —
// colour/chN/NNh/boost/PP-SE — and it is gone. Settings in filenames means two sources of truth,
// and the one you cannot fix later is the one already written on someone's disk.)
inline std::vector<Device> buildDevices (const std::vector<FileMeta>& files)
{
    struct Acc
    {
        Device d;
        std::string gearModel;                             // to re-merge files that omit rig_id
    };
    std::vector<Acc> accs;

    // Group key priority: a stamped rig_id, else the gear_model / family name. A file that omits
    // rig_id but shares a family's gear_model MERGES into it (the spec promises rig_id grouping
    // "survives" — inconsistent stamping across a family must not split it).
    auto accFor = [&accs] (const std::string& rid, const std::string& gearModel,
                           const std::string& family) -> Acc& {
        for (auto& a : accs)
        {
            if (! rid.empty() && a.d.rigId == rid) return a;                     // same explicit id
            if (rid.empty() && ! a.d.rigId.empty() && ! gearModel.empty()
                && a.gearModel == gearModel) return a;                           // adopt into the id'd acc
            if (rid.empty() && a.d.rigId.empty() && a.d.family == family) return a;
        }
        accs.emplace_back();
        accs.back().d.rigId    = rid;
        accs.back().gearModel  = gearModel;
        accs.back().d.family   = family;
        return accs.back();
    };

    for (const auto& f : files)
    {
        const auto metaAt = [&f] (const char* k) {
            const auto it = f.meta.find (k);
            return it == f.meta.end() ? std::string() : it->second;
        };
        const auto spec = metaAt ("controls");
        if (! spec.empty())
        {
            Settings s;
            for (const auto& [k, v] : f.meta)
                if (k.rfind ("settings.", 0) == 0) s[k.substr (9)] = v;
            const auto gm = metaAt ("gear_model");
            const auto family = ! gm.empty() ? gm : f.filenameBase;
            const auto rid = metaAt ("rig_id");
            auto& a = accFor (rid, gm, family);
            if (a.d.rigId.empty() && ! rid.empty()) a.d.rigId = rid;   // a later id'd file names the group
            if (a.gearModel.empty()) a.gearModel = gm;
            if (a.d.slot.empty())
                a.d.slot = ! metaAt ("slot").empty() ? metaAt ("slot") : metaAt ("gear_type");
            if (a.d.controls.empty())
            {
                a.d.controls = parseControlsSpec (spec);
                // …and each dial's rotation from its own flat key, so a manifest-less folder still
                // knows how far the knob turns (the spec string carries values, not the arc).
                for (auto& c : a.d.controls)
                    if (const auto it = f.meta.find ("sweep." + c.name); it != f.meta.end())
                        if (const int deg = detail::degrees (it->second); deg > 0) c.sweep = deg;
            }
            a.d.files.push_back ({ f.id, std::move (s) });
            continue;
        }
        // No `controls` key: a foreign or unstamped file. It still PLAYS — it just has no knobs and
        // no family to join. Grouped by gear_model when it has one, else standing alone under its
        // own name; the name is a caption, never evidence.
        const auto gm = metaAt ("gear_model");
        auto& a = accFor (std::string(), gm, ! gm.empty() ? gm : f.filenameBase);
        if (a.d.slot.empty())
            a.d.slot = ! metaAt ("slot").empty() ? metaAt ("slot") : metaAt ("gear_type");
        a.d.files.push_back ({ f.id, Settings {} });
    }

    // The settings-trim keeps files matchable against the visible controls: a file may carry
    // settings.* for controls the group does not expose, and without trimming find() and the
    // defaults miss while resolve() leaks stray keys.
    auto trimToControls = [] (Device& d) {
        for (auto& fe : d.files)
        {
            Settings kept;
            for (const auto& c : d.controls)
                if (const auto it = fe.settings.find (c.name); it != fe.settings.end()) kept[c.name] = it->second;
            fe.settings = std::move (kept);
        }
    };

    std::vector<Device> out;
    for (auto& a : accs)
    {
        trimToControls (a.d);
        out.push_back (std::move (a.d));
    }
    return out;
}

// The user changed `changed` to `value`: pin it, keep everything else where it is, fall back to
// the closest captured combination. `settings` is updated to the CHOSEN file's real combination.
// Returns nullptr only for a device with no files.
inline const FileEntry* resolve (const Device& d, Settings& settings,
                                 const std::string& changed, const std::string& value)
{
    if (d.files.empty()) return nullptr;
    const bool hadChanged = settings.count (changed) != 0;
    const std::string prevChanged = hadChanged ? settings.at (changed) : std::string();
    settings[changed] = value;
    if (const auto* exact = d.find (settings)) return exact;

    const auto defaults = defaultSettings (d);
    const FileEntry* best = nullptr;
    long bestScore = -1, bestDef = -1;
    for (const auto& f : d.files)
    {
        const auto it = f.settings.find (changed);
        if (it == f.settings.end() || it->second != value) continue;   // the turned control is law
        long score = 0, defMatch = 0;
        for (const auto& c : d.controls)
        {
            if (c.name == changed) continue;
            const auto fv = f.settings.count (c.name) ? f.settings.at (c.name) : std::string();
            const auto rv = settings.count (c.name) ? settings.at (c.name) : std::string();
            const auto dv = defaults.count (c.name) ? defaults.at (c.name) : std::string();
            if (fv == rv) score += 4;                                  // keep what the user had
            else if (fv == dv) score += 1;                             // else prefer the default
            if (fv == dv) ++defMatch;
        }
        // Deterministic: higher score wins; on a TIE, prefer the file sitting on more defaults
        // (the contract's "break ties toward each control's default").
        if (score > bestScore || (score == bestScore && defMatch > bestDef))
        {
            bestScore = score; bestDef = defMatch; best = &f;
        }
    }
    if (best == nullptr)
    {
        // The turned value was never captured on this control. Honour the pin — do NOT return a file
        // that contradicts the user's turn; leave settings as they were and report "no such file".
        if (hadChanged) settings[changed] = prevChanged; else settings.erase (changed);
        return nullptr;
    }
    settings = best->settings;
    return best;
}

// ---------------------------------------------------------------------------------------------
// The rig CHAIN — a pack (.orbitrig) is an ordered list of stages. std-only model; the rig.json
// loader that fills it lives in namz_rig_load.h (that one needs a JSON parser). A player walks the
// chain in signal order and SKIPS any stage whose kind it doesn't understand.
// ---------------------------------------------------------------------------------------------

enum class StageKind { Nam, Ir, Eq, Unknown };

inline StageKind stageKindFrom (const std::string& s)
{
    if (s == "nam") return StageKind::Nam;
    if (s == "ir")  return StageKind::Ir;
    if (s == "eq")  return StageKind::Eq;
    return StageKind::Unknown;
}

// The software-EQ stage carries author GUIDANCE only (the tone stack is always software, never
// captured): optional per-knob defaults, knobs to hide, a simplified single-"TONE" mode, and
// whether to draw the response curve. Absent stage = the player's own default EQ.
struct EqHints
{
    std::string model;                              // e.g. "fmv" (empty = player default)
    std::map<std::string, std::string> defaults;    // knob name -> starting value
    std::vector<std::string> hidden;                // knobs to hide (e.g. "hpf", "lpf")
    bool toneOnly  = false;                          // collapse to one "TONE" knob
    bool showCurve = true;                           // draw the EQ curve
};

// ---------------------------------------------------------------------------------------------
// MEASURED controls — a linear knob the capture tool did NOT turn into a model axis.
//
// A tone control sitting after the clipper is linear: sweeping it changes the frequency response,
// not the distortion. Capturing it as a matrix axis would multiply the take count for nothing, so
// the capture tool measures it instead (a short sweep per position, deconvolved against the
// reference position) and the pack ships the recovered response. The player applies it as DSP and
// gets a CONTINUOUS knob out of a handful of measured points.
//
// Format invariant: every file of the stage was captured with this control AT `reference`, and the
// responses are relative to it. So the reference position is flat by construction — a player that
// ignores `measured` (or a single .namz pulled out of the pack) plays the reference tone, which is
// exactly what its weights encode. No lie, no double-filtering.
// ---------------------------------------------------------------------------------------------

struct MeasuredPosition
{
    std::string value;           // the dial position, degrees ("0" … "300")
    double norm       = 0.0;     // 0..1 across the sweep — a player's knob maps straight onto this
    std::vector<double> db;      // the measured response against the reference, dB per grid point
};

// The log-spaced frequency grid `MeasuredPosition::db` is sampled on. points == 0 → the control is
// unusable and a reader must skip it: the curve IS the description, there is nothing else to fall
// back to. Earlier drafts shipped a fitted 1-pole corner beside it (`lp1_hz` + `gain_db`) and a
// residual to say how far the fit lay from the truth. That was withdrawn after measuring real pedals:
// a Big Muff tone control is a bass-cut/treble-boost blend, a 30 dB tilt across the band, and the
// best 1-pole fit missed it by 15-65 dB. A number that wrong is worse than no number, because a
// player written against it produces confident nonsense. The curve costs a few hundred bytes.
struct MeasuredGrid
{
    double fLo = 20.0, fHi = 20000.0;
    int points = 0;
};

// Where the shipped curve was shown to be a FILTER. Each position is swept at several drive levels;
// a filter comes back the same at all of them, a distortion product does not. Outside this band the
// measurement did not reproduce, so a player must hold the curve at the nearest trusted value rather
// than apply it — on a Big Muff tone control the curve is solid to 9 kHz and disagrees by 4-8 dB by
// 12 kHz, which is hiss and harmonics, not a tone stack.
//
// `levels` == 1 means the control was swept at one level only: nothing was tested, the band spans the
// whole grid, and that is an honest absence of evidence rather than a claim.
//
// `span_db` is what the band is a claim ABOUT: the drive range over which the curve held. Verified over
// 24 dB means a signal swinging 24 dB under the capture level stays inside the promise; a fuzz driven
// 40 dB down goes clean and its bass stops matching, which is the pedal being honest rather than the
// measurement failing. A reader that cares about very quiet playing should treat the band as untested
// there, not as guaranteed.
struct MeasuredTrust
{
    double loHz = 0.0, hiHz = 0.0;            // 0/0 = not stated: trust the whole grid
    double spanDb = 0.0;                      // the DRIVE RANGE it was verified over, dB below full
    int levels = 0;
};

struct Measured
{
    std::string name;                         // the knob, e.g. "tone"
    int sweep = 0;                            // dial rotation, degrees
    std::string placement = "post";           // where the filter goes; unknown value → player SKIPS it
    std::string reference;                    // the position every file of the stage was captured at
    Settings operatingPoint;                  // capture axes held while sweeping (provenance)
    MeasuredGrid grid;
    MeasuredTrust trusted;
    std::vector<MeasuredPosition> positions;  // dial order
};

// ---------------------------------------------------------------------------------------------
// `blend` — a dry/wet mix knob, the third kind of control and neither of the other two.
//
// A blend is not a filter, so it cannot be `measured`; and it must not be a captured axis, because
// capturing it would multiply the take count to ship something the player already has for free. The dry
// signal IS the DI the model is being fed. Eleven blend positions as an axis would be eleven copies of
// one model plus arithmetic.
//
// So the pack describes the mix instead:
//
//     out = polarity · dry_gain(pos) · (DI * dry) + wet_gain(pos) · model(DI)
//
// `dry` is the dry path's own response, measured — on a bass pedal it is almost never flat, because the
// whole point of the circuit is to keep a clean low end out of the distortion. It is measured with the
// knob at `dry_end`, where the output is the dry path ALONE and therefore linear; that measurement is
// exact, and it is why a blend can be described from outside the box at all.
//
// `polarity` is the sign of the dry path against the wet one. It matters more than it looks: two paths
// summed out of phase gut the middle of the knob's travel, and a player that assumed + would render the
// centre position hollow. -1 says the box inverts one of them.
//
// `law` names how the two gains were derived (`linear`, `equal_power`, or anything else) — provenance
// only, never something a reader must implement, because the gains themselves are shipped per position.
// A pot with a peculiar taper is expressible by simply carrying its numbers.
//
// Invariant, the same one `measured` has: every model of the stage was captured with the blend at
// `reference` — the full-WET end. So a player that ignores `blend`, or a lone .namz pulled out of the
// pack, plays full wet, which is exactly what those weights encode. Nothing is double-mixed.
struct BlendPosition
{
    std::string value;           // the knob position
    double norm   = 0.0;         // 0..1 across the sweep
    double dryDb  = 0.0;         // the dry path's gain here, dB (-120 = silent)
    double wetDb  = 0.0;         // the model's gain here, dB
};

struct Blend
{
    std::string name;                         // the knob, e.g. "blend"
    int sweep = 0;                            // dial rotation, degrees; 0 = a switch
    std::string reference;                     // where the models were captured: the full-wet end
    std::string dryEnd;                        // where the dry curve was measured: the full-dry end
    std::string law = "linear";                // how the gains below were derived (provenance)
    int polarity = 1;                          // +1, or -1 if the box inverts the dry path
    Settings operatingPoint;                   // capture axes held while sweeping (provenance)
    MeasuredGrid grid;
    MeasuredTrust trusted;                     // where `dry` is a filter — same rules as `measured`
    std::vector<double> dryDb;                 // the dry path's response, dB per grid point
    std::vector<BlendPosition> positions;      // knob order
};

struct Stage
{
    StageKind kind = StageKind::Unknown;
    std::string rawKind;                          // the original string (preserved for unknown kinds)
    std::string slot;                               // pedal | preamp | amp | poweramp | rig
    std::string make, model, gearType;              // gear caption
    // What this device sounds like AT ITS DEFAULT POSITION (mid-sweep gain — the combination a
    // player loads), on the ordered scale `clean < edge < crunch < hi-gain`. One honest label rather
    // than a range: both ends of a range are eyeballed anyway, and a range invites a player to fake
    // a per-position character it was never given. Per-FILE nuance stays available as the
    // conventional `tone_type` header key. The scale measures gain AMOUNT only — voicing/era words
    // belong to a separate dimension, never mixed in, or the ordering it exists for stops working.
    std::string toneType;
    // What KIND of dirt, independent of how much: `od` | `dist` | `fuzz` (empty = unstated, which is
    // the honest answer for a clean preamp). A second axis rather than more `tone_type` values: a
    // fuzz and a metal distortion sit at the same gain amount and sound nothing alike, so folding
    // them into one ordered scale would destroy the ordering AND lose the distinction.
    std::string voicing;
    // What is IN the captured signal path: `type:count` in signal order — "tube:4", "ic:1,diode:2",
    // "tube:1,bjt:1". Documentation a player renders as a glyph row, not something the DSP reads.
    // Per FILE the same string ships under the header's long-standing `device` key; here it is named
    // `circuit` because a stage's `device` is already its selectable matrix.
    std::string circuit;
    Device device;                                  // Nam: controls + files (the selectable matrix)
    std::vector<Measured> measured;                 // Nam: linear knobs shipped as DSP, not as axes
    std::vector<Blend> blend;                       // Nam: dry/wet mix knobs — the player mixes, see above
    std::vector<std::string> irFiles;               // Ir: cabinet impulse file names
    EqHints eq;                                     // Eq: the guidance above
};

struct Rig
{
    std::string rigId, name, modeledBy;
    // The physical box several packs came from. A three-channel head ships as three packs — each its
    // own device, with its own rig_id — and this is the only thing that says they are siblings. It
    // GROUPS in a list and does nothing else: sharing a rig_id would merge them back into one device
    // with one matrix, which is exactly the pretence splitting them was meant to end.
    std::string familyId;
    std::vector<Stage> chain;                        // signal order

    // First stage the player can actually run (kind != Unknown), or nullptr.
    const Stage* firstKnown() const
    {
        for (const auto& s : chain) if (s.kind != StageKind::Unknown) return &s;
        return nullptr;
    }
};

} // namespace namz::rig

#endif // NAMZ_RIG_H
