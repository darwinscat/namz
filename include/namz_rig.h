// SPDX-License-Identifier: MIT
// namz_rig — the .namz DEVICE MODEL: turn a family of .nam/.namz files into a device with working
// controls, and answer "the user turned one knob — which file plays now?". The player-side twin of
// the capture-side conventions in NAMZ-FORMAT.md ("Capture-identity keys"): controls come from the
// `controls` spec + per-file `settings.*` metadata (read them cheaply with namz::readMeta), with a
// FILENAME-TOKEN FALLBACK for legacy files (the OrbitCab grammar: colour/chN channel, NNh gain,
// `boost`, PP/SE topology) so packs captured before the convention keep working.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.
//
// Header-only, C++17, std-only (no nlohmann, no namz.h dependency — metadata arrives as an
// already-parsed map). Message-thread only; selection results are stable file ids the caller maps
// back to its own entries.
//
// Selection policy (generalized from OrbitCab's PreampSelector): pin the control the user just
// changed, keep every other control where it is; when that exact combination was never captured,
// pick the candidate matching the most remaining controls, breaking ties toward each control's
// default (noon gain · boost off · first value) and then capture order. The returned file's actual
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

struct Control
{
    std::string name;
    Role role = Role::Generic;
    std::vector<std::string> values;   // capture/dial order
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
// Legacy filename tokens (the OrbitCab grammar) — the fallback for files without metadata
// ---------------------------------------------------------------------------------------------

namespace detail
{
    inline bool isColour (const std::string& l)
    {
        for (const char* c : { "red", "green", "blue", "yellow", "orange", "purple", "white" })
            if (l == c) return true;
        return false;
    }
    inline bool isChN (const std::string& l)
    {
        return l.size() == 3 && l.rfind ("ch", 0) == 0 && l[2] >= '1' && l[2] <= '4';
    }
    inline bool isClock (const std::string& l)
    {
        if (l.size() < 2 || l.size() > 3 || l.back() != 'h') return false;
        for (std::size_t i = 0; i + 1 < l.size(); ++i)
            if (! std::isdigit ((unsigned char) l[i])) return false;
        return true;
    }
    inline int clockHours (const std::string& v)   // "07h" → 7; non-clock → -1
    {
        const auto l = lower (trim (v));
        return isClock (l) ? std::stoi (l.substr (0, l.size() - 1)) : -1;
    }
    inline bool isTopology (const std::string& l) { return l == "pp" || l == "se"; }
    inline bool isFalsy (const std::string& l)
    {
        return l == "off" || l == "no" || l == "false" || l == "0" || l.empty();
    }

    // Split a basename on the token separators (space/dash/underscore).
    inline std::vector<std::string> tokens (const std::string& base)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : base)
        {
            if (ch == ' ' || ch == '-' || ch == '_')
            {
                if (! cur.empty()) { out.push_back (cur); cur.clear(); }
            }
            else cur += ch;
        }
        if (! cur.empty()) out.push_back (cur);
        return out;
    }
} // namespace detail

// One file going INTO the model: a stable id (path/basename — echoed back by selection), the
// basename for the legacy fallback + display, and the metadata map (empty for legacy files).
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

// Per-control default: noon-most gain, falsy boost, first value otherwise.
inline std::string defaultValue (const Control& c)
{
    if (c.values.empty()) return {};
    if (c.role == Role::Gain)
    {
        std::string best = c.values.front();
        int bestD = 1 << 30;
        for (const auto& v : c.values)
            if (const int h = detail::clockHours (v); h >= 0 && std::abs (h - 12) < bestD)
            {
                bestD = std::abs (h - 12);
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

namespace detail
{
    // Legacy: synthesize settings from filename tokens; returns the family (leftover words).
    inline std::string legacyParse (const std::string& base, Settings& s)
    {
        std::string family;
        bool boostSeen = false;
        for (const auto& tok : tokens (base))
        {
            const auto l = lower (tok);
            if (isColour (l) || isChN (l)) s["channel"] = l;
            else if (isClock (l))          s["gain"] = l;
            else if (l == "boost")         { s["boost"] = "on"; boostSeen = true; }
            else if (isTopology (l))       s["topology"] = tok;
            else family += (family.empty() ? "" : " ") + tok;
        }
        if (! boostSeen) s["boost"] = "off";
        return family.empty() ? base : family;
    }

    // True when EVERY token of the basename is a recognized control token (no leftover name word) —
    // buildDevices then groups such files together instead of making each its own device.
    inline bool legacyIsAllTokens (const std::string& base)
    {
        const auto ts = tokens (base);
        if (ts.empty()) return false;
        for (const auto& tok : ts)
        {
            const auto l = lower (tok);
            if (! (isColour (l) || isChN (l) || isClock (l) || l == "boost" || isTopology (l)))
                return false;
        }
        return true;
    }

    inline void addValue (std::vector<std::string>& values, const std::string& v)
    {
        if (std::find (values.begin(), values.end(), v) == values.end()) values.push_back (v);
    }
} // namespace detail

// Build devices from a set of files. Grouping key: `rig_id` when stamped, else the family name
// (metadata `gear_model`, else the filename leftovers). Meta-driven files take their control spec
// verbatim (first spec seen wins); legacy files grow a channel/gain/boost/topology control per
// dimension that shows ≥2 distinct values (single-valued dimensions stay invisible — OrbitCab's
// rule), with gains sorted by the clock and boost normalized to off|on.
inline std::vector<Device> buildDevices (const std::vector<FileMeta>& files)
{
    struct Acc
    {
        Device d;
        bool metaDriven = false;
        std::string gearModel;                             // to re-merge files that omit rig_id
        std::vector<std::string> channels, gains, topos;   // legacy dimension unions, insertion order
        bool anyBoostOn = false, anyBoostOff = false;
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
            a.metaDriven = true;
            if (a.d.rigId.empty() && ! rid.empty()) a.d.rigId = rid;   // a later id'd file names the group
            if (a.gearModel.empty()) a.gearModel = gm;
            if (a.d.slot.empty())
                a.d.slot = ! metaAt ("slot").empty() ? metaAt ("slot") : metaAt ("gear_type");
            if (a.d.controls.empty()) a.d.controls = parseControlsSpec (spec);
            a.d.files.push_back ({ f.id, std::move (s) });
            continue;
        }
        // legacy filename fallback. When the basename is ALL tokens (no leftover name), group them
        // under one key so a nameless legacy pack still forms ONE device (else every combination
        // splits into its own control-less device).
        Settings s;
        std::string family = detail::legacyParse (f.filenameBase, s);
        std::string groupFamily = family;
        if (family == f.filenameBase && detail::legacyIsAllTokens (f.filenameBase))
            groupFamily = std::string();                              // token-only: shared empty key
        auto& a = accFor (std::string(), std::string(), groupFamily);
        detail::addValue (a.channels, s.count ("channel") ? s["channel"] : std::string());
        detail::addValue (a.gains,    s.count ("gain") ? s["gain"] : std::string());
        detail::addValue (a.topos,    s.count ("topology") ? s["topology"] : std::string());
        (s["boost"] == "on" ? a.anyBoostOn : a.anyBoostOff) = true;
        a.d.files.push_back ({ f.id, std::move (s) });
    }

    // The settings-trim keeps files matchable against the visible controls — needed for BOTH paths
    // (meta files carry settings.* for controls the group may not expose; without trimming, find()
    // and defaults miss and resolve() leaks stray keys).
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
        if (! a.metaDriven)
        {
            auto strip = [] (std::vector<std::string>& v) { v.erase (std::remove (v.begin(), v.end(), std::string()), v.end()); };
            strip (a.channels); strip (a.gains); strip (a.topos);
            std::sort (a.gains.begin(), a.gains.end(),
                       [] (const std::string& x, const std::string& y) { return detail::clockHours (x) < detail::clockHours (y); });
            if (a.channels.size() > 1) a.d.controls.push_back ({ "channel", Role::Channel, a.channels });
            if (a.topos.size() > 1)    a.d.controls.push_back ({ "topology", Role::Topology, a.topos });
            if (a.anyBoostOn && a.anyBoostOff) a.d.controls.push_back ({ "boost", Role::Boost, { "off", "on" } });
            if (a.gains.size() > 1)    a.d.controls.push_back ({ "gain", Role::Gain, a.gains });
        }
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

struct Stage
{
    StageKind kind = StageKind::Unknown;
    std::string rawKind;                            // the original string (preserved for unknown kinds)
    std::string slot;                               // pedal | preamp | amp | poweramp | rig
    std::string make, model, gearType;              // gear caption
    Device device;                                  // Nam: controls + files (the selectable matrix)
    std::vector<std::string> irFiles;               // Ir: cabinet impulse file names
    EqHints eq;                                     // Eq: the guidance above
};

struct Rig
{
    std::string rigId, name, modeledBy;
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
