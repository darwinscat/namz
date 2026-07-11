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
        std::vector<std::string> channels, gains, topos;   // legacy dimension unions, insertion order
        bool anyBoostOn = false, anyBoostOff = false;
    };
    std::vector<Acc> accs;

    auto accFor = [&accs] (const std::string& key, const std::string& family) -> Acc& {
        for (auto& a : accs)
            if ((! a.d.rigId.empty() && a.d.rigId == key) || (a.d.rigId.empty() && a.d.family == key))
                return a;
        accs.emplace_back();
        accs.back().d.rigId  = key == family ? std::string() : key;
        accs.back().d.family = family;
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
            const auto family = ! metaAt ("gear_model").empty() ? metaAt ("gear_model") : f.filenameBase;
            const auto rid = metaAt ("rig_id");
            auto& a = accFor (! rid.empty() ? rid : family, family);
            a.metaDriven = true;
            if (a.d.rigId.empty()) a.d.rigId = rid;
            if (a.d.slot.empty())
                a.d.slot = ! metaAt ("slot").empty() ? metaAt ("slot") : metaAt ("gear_type");
            if (a.d.controls.empty()) a.d.controls = parseControlsSpec (spec);
            a.d.files.push_back ({ f.id, std::move (s) });
            continue;
        }
        // legacy filename fallback
        Settings s;
        const auto family = detail::legacyParse (f.filenameBase, s);
        auto& a = accFor (family, family);
        detail::addValue (a.channels, s.count ("channel") ? s["channel"] : std::string());
        detail::addValue (a.gains,    s.count ("gain") ? s["gain"] : std::string());
        detail::addValue (a.topos,    s.count ("topology") ? s["topology"] : std::string());
        (s["boost"] == "on" ? a.anyBoostOn : a.anyBoostOff) = true;
        a.d.files.push_back ({ f.id, std::move (s) });
    }

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
            // Trim per-file settings down to the visible controls so exact matches work.
            for (auto& fe : a.d.files)
            {
                Settings kept;
                for (const auto& c : a.d.controls)
                    if (const auto it = fe.settings.find (c.name); it != fe.settings.end()) kept[c.name] = it->second;
                fe.settings = std::move (kept);
            }
        }
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
    settings[changed] = value;
    if (const auto* exact = d.find (settings)) return exact;

    const auto defaults = defaultSettings (d);
    const FileEntry* best = nullptr;
    long bestScore = -1;
    for (const auto& f : d.files)
    {
        const auto it = f.settings.find (changed);
        if (it == f.settings.end() || it->second != value) continue;   // the turned control is law
        long score = 0;
        for (const auto& c : d.controls)
        {
            if (c.name == changed) continue;
            const auto fv = f.settings.count (c.name) ? f.settings.at (c.name) : std::string();
            const auto rv = settings.count (c.name) ? settings.at (c.name) : std::string();
            const auto dv = defaults.count (c.name) ? defaults.at (c.name) : std::string();
            if (fv == rv) score += 4;                                  // keep what the user had
            else if (fv == dv) score += 1;                             // else prefer the default
        }
        if (score > bestScore) { bestScore = score; best = &f; }
    }
    if (best == nullptr) best = &d.files.front();                      // value never captured — bail sanely
    settings = best->settings;
    return best;
}

} // namespace namz::rig

#endif // NAMZ_RIG_H
