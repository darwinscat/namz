// SPDX-License-Identifier: MIT
// namz_rig_write — the WRITER side of the .orbitrig contract, twin of namz_rig_load.h: a capture
// tool builds its per-file header metadata (stampMeta → the flat "Capture-identity keys" of
// NAMZ-FORMAT.md) and the pack manifest (writeManifest → rig.json text) from the SAME Rig/Stage/
// Device vocabulary the players read with. Byte compatibility is then a property of the library,
// not of two codebases keeping their string literals in sync.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.
//
// Round-trip contract (covered by tests/rig.cpp): for every field this header writes,
// loadRigManifest (writeManifest (rig)) reproduces the Rig, and buildDevices() over stampMeta()
// output reconstructs the device. Fields the model does not carry (unknown stage extras, foreign
// JSON keys) are NOT preserved — writers own their output, readers stay tolerant.
//
// Split like the loader: needs a JSON writer (nlohmann — the same dep namz.h/namz_rig_load.h use),
// while the core model (namz_rig.h) stays std-only.

#ifndef NAMZ_RIG_WRITE_H
#define NAMZ_RIG_WRITE_H

#include "namz_rig.h"

#include <nlohmann/json.hpp>

#include <map>
#include <cmath>
#include <string>

namespace namz::rig
{

inline const char* stageKindToString (StageKind k)
{
    switch (k)
    {
        case StageKind::Nam: return "nam";
        case StageKind::Ir:  return "ir";
        case StageKind::Eq:  return "eq";
        case StageKind::Unknown: break;
    }
    return "";
}

// The flat header keys ONE file carries (NAMZ-FORMAT.md "Capture-identity keys" + the conventional
// display fields): settings.<control> for this file's positions, the whole device's `controls`
// spec, rig_id / slot / gear_* / modeled_by identity, and the conventional `boost` flag ("true"/
// "false" — the codec types it to a bool at pack time) derived from the Boost-role control's value.
// Feed the result to the packer's metadata option verbatim.
inline std::map<std::string, std::string> stampMeta (const Rig& rig, const Stage& stage,
                                                     const Settings& fileSettings)
{
    std::map<std::string, std::string> m;
    if (! stage.make.empty())     m["gear_make"]  = stage.make;
    if (! stage.model.empty())    m["gear_model"] = stage.model;
    if (! stage.gearType.empty()) m["gear_type"]  = stage.gearType;
    // The particular box, stamped per FILE too — a single .namz pulled out of the pack and handed
    // to another player still says which unit it came off, which is the whole point of stamping.
    if (stage.year > 0)             m["gear_year"]   = std::to_string (stage.year);
    if (! stage.serialNumber.empty()) m["gear_serial_number"] = stage.serialNumber;
    if (! stage.designedIn.empty()) m["designed_in"] = stage.designedIn;
    if (! stage.madeIn.empty())     m["made_in"]     = stage.madeIn;
    if (! stage.slot.empty())     m["slot"]       = stage.slot;
    if (! rig.modeledBy.empty())  m["modeled_by"] = rig.modeledBy;

    const auto rid = ! stage.device.rigId.empty() ? stage.device.rigId : rig.rigId;
    if (! rid.empty())            m["rig_id"]     = rid;

    // Stage-level facts every file of the stage shares. They are stamped per file so a single .namz
    // pulled out of the pack still says what it is — the same reason its settings and controls are
    // here. `circuit` ships under the header's long-standing `device` key, which players already read.
    if (! rig.familyId.empty())   m["family_id"]  = rig.familyId;
    if (! stage.toneType.empty()) m["tone_type"]  = stage.toneType;
    if (! stage.voicing.empty())  m["voicing"]    = stage.voicing;
    if (! stage.circuit.empty())  m["device"]     = stage.circuit;

    if (const auto spec = buildControlsSpec (stage.device.controls); ! spec.empty())
        m["controls"] = spec;
    for (const auto& [k, v] : fileSettings)
        m["settings." + k] = v;

    // How far each DIAL turns, in degrees — its `values` are positions on that arc, so a player can
    // place the knob without knowing the device. Flat per-control key like settings.*: additive, a
    // reader that ignores it still selects correctly.
    for (const auto& c : stage.device.controls)
        if (c.sweep > 0) m["sweep." + c.name] = std::to_string (c.sweep);

    for (const auto& c : stage.device.controls)
        if (c.role == Role::Boost)
        {
            const auto it = fileSettings.find (c.name);
            const bool on = it != fileSettings.end()
                            && ! detail::isFalsy (detail::lower (detail::trim (it->second)));
            m["boost"] = on ? "true" : "false";
            break;
        }
    return m;
}

// ---------------------------------------------------------------------------------------------
// THE LAYOUT of rig.json. A manifest is read by machines and inspected by people, and a generic
// pretty-printer serves only the first: every key on its own line, every number of a curve on its
// own line, keys in alphabetical order (so `chain` came before `format` and a band's `type` came
// last). This emitter writes the same JSON the way a person lays it out:
//
//   * keys in the order the format lists them — `ordered_json` keeps insertion order, and the writer
//     inserts in that order;
//   * a LEAF object — one whose values are scalars, arrays of scalars, or objects of scalars — on ONE
//     line: a control, a file entry, a band, a position, `gear`, `grid`, `trusted`;
//   * an array of scalars inline, wrapped at `width` with the continuation aligned under its first
//     element — a dial's twenty-one values, a curve's points;
//   * inside the objects whose shape the format fixes, the scalars that belong together share a line
//     (`"format", "schema"` / `"rig_id", "name", "modeled_by"`; a knob's `"name", "sweep",
//     "placement"` / `"reference", "default"`); everything else gets a line of its own.
//
// Nothing here changes a value: numbers are spelled by nlohmann exactly as before, and a reader that
// parses JSON — which is every reader — sees the same document. Only the bytes of the golden packs
// move, and the conformance test regenerates them from the same writer.
// ---------------------------------------------------------------------------------------------
namespace detail
{
    using OJ = nlohmann::ordered_json;

    inline bool isScalar (const OJ& v) { return ! v.is_structured(); }
    inline bool isScalarArray (const OJ& v)
    {
        if (! v.is_array()) return false;
        for (const auto& e : v) if (! isScalar (e)) return false;
        return true;
    }
    inline bool isScalarObject (const OJ& v)
    {
        if (! v.is_object()) return false;
        for (const auto& kv : v.items()) if (! isScalar (kv.value())) return false;
        return true;
    }
    inline bool isLeaf (const OJ& v)
    {
        if (! v.is_object()) return false;
        for (const auto& kv : v.items())
            if (! (isScalar (kv.value()) || isScalarArray (kv.value()) || isScalarObject (kv.value()))) return false;
        return true;
    }

    // The scalars that share a line, by the key the object sits under ("" = the manifest itself).
    inline std::vector<std::vector<std::string>> lineGroups (const std::string& under)
    {
        if (under.empty())     return { { "format", "schema" }, { "rig_id", "family_id", "name", "modeled_by" } };
        if (under == "chain")  return { { "kind", "slot" }, { "tone_type", "voicing", "circuit" },
                                        { "model", "tone_only", "show_curve" } };
        if (under == "tone")   return { { "name", "sweep", "placement" }, { "reference", "default" } };
        if (under == "blend")  return { { "name", "sweep", "reference", "dry_end" },
                                        { "default", "law", "gains_measured", "polarity" } };
        return {};
    }
    inline int groupOf (const std::vector<std::vector<std::string>>& groups, const std::string& key)
    {
        for (std::size_t g = 0; g < groups.size(); ++g)
            for (const auto& k : groups[g]) if (k == key) return (int) g;
        return -1;
    }

    inline std::size_t column (const std::string& out)
    {
        const auto nl = out.rfind ('\n');
        return nl == std::string::npos ? out.size() : out.size() - nl - 1;
    }

    // One line, wrapped only inside arrays of scalars.
    inline void emitInline (std::string& out, const OJ& v, std::size_t width)
    {
        if (isScalar (v)) { out += v.dump(); return; }
        if (v.is_array())
        {
            out += '[';
            const auto align = column (out);
            bool first = true;
            for (const auto& e : v)
            {
                const auto piece = e.dump();
                if (! first)
                {
                    out += ',';
                    if (column (out) + 1 + piece.size() > width) { out += '\n'; out += std::string (align, ' '); }
                    else out += ' ';
                }
                out += piece;
                first = false;
            }
            out += ']';
            return;
        }
        // an object of scalars, or a leaf: `{ "k": v, "k2": v2 }`. A member that would run past the
        // width starts a new line, aligned under the first member — so a position breaks before its
        // `db`, not inside it, and an array wraps within itself only when it is too long on its own.
        if (v.empty()) { out += "{}"; return; }
        out += "{ ";
        const auto align = column (out);
        bool first = true;
        for (const auto& kv : v.items())
        {
            std::string key = OJ (kv.key()).dump(); key += ": ";
            std::string measure; emitInline (measure, kv.value(), std::string::npos);   // unwrapped
            if (! first)
            {
                out += ',';
                if (column (out) + 1 + key.size() + measure.size() + 2 > width) { out += '\n'; out += std::string (align, ' '); }
                else out += ' ';
            }
            out += key;
            emitInline (out, kv.value(), width);
            first = false;
        }
        out += " }";
    }

    inline void emit (std::string& out, const OJ& v, const std::string& under, int depth, int step, std::size_t width)
    {
        if (isScalar (v) || isScalarArray (v) || isScalarObject (v) || isLeaf (v)) { emitInline (out, v, width); return; }
        const std::string ind (std::size_t (depth * step), ' '), inner (std::size_t ((depth + 1) * step), ' ');
        if (v.is_array())
        {
            out += "[\n";
            bool first = true;
            for (const auto& e : v)
            {
                if (! first) out += ",\n";
                out += inner;
                emit (out, e, under, depth + 1, step, width);
                first = false;
            }
            out += '\n'; out += ind; out += ']';
            return;
        }
        // a multi-line object: grouped scalars share a line, everything else has its own
        const auto groups = lineGroups (under);
        out += "{\n";
        int openGroup = -1;
        bool first = true;
        for (const auto& kv : v.items())
        {
            const int g = isScalar (kv.value()) ? groupOf (groups, kv.key()) : -1;
            if (! first)
            {
                if (g >= 0 && g == openGroup) out += ", ";
                else { out += ",\n"; out += inner; }
            }
            else out += inner;
            openGroup = g;
            out += OJ (kv.key()).dump(); out += ": ";
            emit (out, kv.value(), kv.key(), depth + 1, step, width);
            first = false;
        }
        out += '\n'; out += ind; out += '}';
    }
} // namespace detail

// rig.json text — the pack's source of truth (loadRigManifest is the exact inverse for every field
// the model carries). Empty strings are omitted; an Unknown stage writes its preserved rawKind so
// a pack passing through an old tool never loses stages it didn't understand. Laid out for a reader
// with eyes — see detail::emit above; `indent` is the step.
inline std::string writeManifest (const Rig& rig, int indent = 2)
{
    nlohmann::ordered_json j;
    j["format"] = "orbitrig";
    j["schema"] = kRigSchema;   // never a literal: the writer and the reader must bump together
    if (! rig.rigId.empty())     j["rig_id"]     = rig.rigId;
    if (! rig.familyId.empty())  j["family_id"]  = rig.familyId;
    if (! rig.name.empty())      j["name"]       = rig.name;
    if (! rig.modeledBy.empty()) j["modeled_by"] = rig.modeledBy;
    if (! rig.picture.empty())   j["picture"]    = rig.picture;

    auto chain = nlohmann::ordered_json::array();
    for (const auto& st : rig.chain)
    {
        nlohmann::ordered_json sj;
        const auto kind = st.kind != StageKind::Unknown ? std::string (stageKindToString (st.kind))
                                                        : st.rawKind;
        if (kind.empty()) continue;   // a stage with no kind at all cannot be represented
        sj["kind"] = kind;
        if (! st.slot.empty()) sj["slot"] = st.slot;
        if (! st.make.empty() || ! st.model.empty() || ! st.gearType.empty())
        {
            nlohmann::ordered_json g;
            if (! st.make.empty())       g["make"]  = st.make;
            if (! st.model.empty())      g["model"] = st.model;
            if (! st.gearType.empty())   g["type"]  = st.gearType;
            if (st.year > 0)             g["year"]        = st.year;
            if (! st.serialNumber.empty()) g["serial_number"] = st.serialNumber;
            if (! st.designedIn.empty()) g["designed_in"] = st.designedIn;
            if (! st.madeIn.empty())     g["made_in"]     = st.madeIn;
            sj["gear"] = std::move (g);
        }
        if (! st.toneType.empty()) sj["tone_type"] = st.toneType;
        if (! st.voicing.empty())  sj["voicing"]   = st.voicing;
        if (! st.circuit.empty())  sj["circuit"]   = st.circuit;

        if (st.kind == StageKind::Nam)
        {
            auto controls = nlohmann::ordered_json::array();
            for (const auto& c : st.device.controls)
            {
                nlohmann::ordered_json cj;
                cj["name"]   = c.name;
                cj["role"]   = roleToString (c.role);
                if (c.sweep > 0) cj["sweep"] = c.sweep;
                cj["values"] = c.values;
                if (! c.labels.empty()) cj["labels"] = c.labels;
                controls.push_back (std::move (cj));
            }
            if (! controls.empty()) sj["controls"] = std::move (controls);

            auto files = nlohmann::ordered_json::array();
            for (const auto& fe : st.device.files)
            {
                nlohmann::ordered_json fj;
                fj["file"] = fe.id;
                nlohmann::ordered_json s = nlohmann::ordered_json::object();
                for (const auto& [k, v] : fe.settings) s[k] = v;
                fj["settings"] = std::move (s);
                if (fe.inputDb < -0.0005 || fe.inputDb > 0.0005)
                    fj["input_db"] = std::round (fe.inputDb * 100.0) / 100.0;
                if (fe.lagSamples) fj["lag_samples"] = *fe.lagSamples;   // 0 is a reading; absent is none
                files.push_back (std::move (fj));
            }
            if (! files.empty()) sj["files"] = std::move (files);

            // The tone (linear) knobs — see namz_rig.h. ONE FORM PER KNOB: `sections` when the model has
            // them, else `positions`, never both — a reader refuses a manifest that carries both for one
            // knob, so a writer that emitted both would produce a pack nothing can load. `sections` wins
            // because it is the form a producer CHOSE: the ladder is what the bands were fitted to, and
            // a model still holding it beside the fit has not un-chosen anything.
            auto tone = nlohmann::ordered_json::array();
            for (const auto& me : st.tone)
            {
                if (me.name.empty() || (me.positions.empty() && me.sections.empty())) continue;
                const bool asSections = ! me.sections.empty();
                // …and a `sections` knob with no sweep is one the reader drops on sight (the travel law
                // runs on rotation), so writing it would only make a pack that loses a knob in transit.
                if (asSections && me.sweep <= 0) continue;
                nlohmann::ordered_json mj;
                mj["name"] = me.name;
                if (me.sweep > 0)            mj["sweep"]     = me.sweep;
                if (! me.placement.empty())  mj["placement"] = me.placement;
                if (! me.reference.empty())  mj["reference"] = me.reference;
                // The loader has always read this; the writer did not emit it, so a pack that stated
                // where the knob starts lost that fact the first time anything rewrote it — silently,
                // because a missing `default` legally means "start at the reference". Conformance
                // missed it for one reason: no fixture set the field. One now does.
                if (! me.defaultValue.empty()) mj["default"] = me.defaultValue;
                if (! me.operatingPoint.empty())
                {
                    nlohmann::ordered_json op = nlohmann::ordered_json::object();
                    for (const auto& [k, v] : me.operatingPoint) op[k] = v;
                    mj["operating_point"] = std::move (op);
                }
                // The grid and the curves travel together or not at all: a curve without the grid it
                // was sampled on cannot be applied to anything — and a grid without curves says nothing,
                // so a `sections` knob carries none.
                if (! asSections && me.grid.points > 0)
                {
                    nlohmann::ordered_json g;
                    g["f_lo"]   = me.grid.fLo;
                    g["f_hi"]   = me.grid.fHi;
                    g["points"] = me.grid.points;
                    mj["grid"] = std::move (g);
                }
                // Emitted only when levels were actually COMPARED — two or more. A single sweep per
                // position proves nothing about drive, and the block it used to emit then said so in
                // six fields that were always the same six values (whole grid, span 0, levels 1).
                // Absence is the shorter way to say "never tested"; an empty band with levels >= 2
                // still ships, because "tested and failed everywhere" is a different statement.
                if (me.trusted.levels >= 2)
                {
                    nlohmann::ordered_json t;
                    t["lo_hz"]    = me.trusted.loHz;
                    t["hi_hz"]    = me.trusted.hiHz;
                    t["lo_index"] = me.trusted.loIndex;
                    t["hi_index"] = me.trusted.hiIndex;
                    t["span_db"]  = me.trusted.spanDb;
                    t["levels"]   = me.trusted.levels;
                    mj["trusted"] = std::move (t);
                }
                if (asSections)
                {
                    auto sections = nlohmann::ordered_json::array();
                    for (const auto& sc : me.sections)
                    {
                        nlohmann::ordered_json x;
                        x["type"]     = sectionKindToString (sc.kind);
                        x["hz"]       = sc.hz;
                        x["q"]        = sc.q;
                        x["range_db"] = nlohmann::ordered_json::array ({ sc.dbAtMin, sc.dbAtMax });
                        // Only a tilt hinges; writing a pivot on a shelf would invite a reader to apply it.
                        if (sc.kind == SectionKind::Tilt) x["pivot"] = sc.pivot;
                        // A travel is written as the pair at the two stops. A side the model left at 0
                        // ("does not move") is written as the reference value itself — the same statement
                        // in the pack's vocabulary, where an absent KEY means neither side moves and a
                        // present one must say both.
                        if (sc.hzAtMin > 0.0 || sc.hzAtMax > 0.0)
                            x["hz_at"] = nlohmann::ordered_json::array ({ sc.hzAtMin > 0.0 ? sc.hzAtMin : sc.hz,
                                                                  sc.hzAtMax > 0.0 ? sc.hzAtMax : sc.hz });
                        if (sc.qAtMin > 0.0 || sc.qAtMax > 0.0)
                            x["q_at"]  = nlohmann::ordered_json::array ({ sc.qAtMin > 0.0 ? sc.qAtMin : sc.q,
                                                                  sc.qAtMax > 0.0 ? sc.qAtMax : sc.q });
                        sections.push_back (std::move (x));
                    }
                    mj["sections"] = std::move (sections);
                }
                else
                {
                    auto positions = nlohmann::ordered_json::array();
                    for (const auto& p : me.positions)
                    {
                        nlohmann::ordered_json pj;
                        pj["value"]    = p.value;
                        if (! p.label.empty()) pj["label"] = p.label;
                        pj["norm"]     = p.norm;
                        pj["level_db"] = p.levelDb;
                        pj["db"]       = p.db;
                        positions.push_back (std::move (pj));
                    }
                    mj["positions"] = std::move (positions);
                }
                tone.push_back (std::move (mj));
            }
            if (! tone.empty()) sj["tone"] = std::move (tone);

            auto blends = nlohmann::ordered_json::array();
            for (const auto& bl : st.blend)
            {
                if (bl.name.empty()) continue;
                nlohmann::ordered_json bj;
                bj["name"] = bl.name;
                if (bl.sweep > 0)             bj["sweep"]     = bl.sweep;
                if (! bl.reference.empty())   bj["reference"] = bl.reference;
                if (! bl.dryEnd.empty())       bj["dry_end"] = bl.dryEnd;
                if (! bl.defaultValue.empty()) bj["default"] = bl.defaultValue;
                if (! bl.law.empty())         bj["law"]       = bl.law;
                bj["gains_measured"] = bl.gainsMeasured;
                bj["polarity"] = bl.polarity;
                if (! bl.operatingPoint.empty())
                {
                    nlohmann::ordered_json op = nlohmann::ordered_json::object();
                    for (const auto& [k, v] : bl.operatingPoint) op[k] = v;
                    bj["operating_point"] = std::move (op);
                }
                if (bl.grid.points > 0)
                {
                    nlohmann::ordered_json g;
                    g["f_lo"]   = bl.grid.fLo;
                    g["f_hi"]   = bl.grid.fHi;
                    g["points"] = bl.grid.points;
                    bj["grid"] = std::move (g);
                }
                if (bl.trusted.levels >= 2)   // …and the same for a blend
                {
                    nlohmann::ordered_json t;
                    t["lo_hz"]    = bl.trusted.loHz;
                    t["hi_hz"]    = bl.trusted.hiHz;
                    t["lo_index"] = bl.trusted.loIndex;
                    t["hi_index"] = bl.trusted.hiIndex;
                    t["span_db"]  = bl.trusted.spanDb;
                    t["levels"]   = bl.trusted.levels;
                    bj["trusted"] = std::move (t);
                }
                bj["dry"]          = bl.dryDb;
                bj["dry_level_db"] = bl.dryLevelDb;
                auto bp = nlohmann::ordered_json::array();
                for (const auto& p : bl.positions)
                {
                    nlohmann::ordered_json pj;
                    pj["value"]  = p.value;
                    if (! p.label.empty()) pj["label"] = p.label;
                    pj["norm"]   = p.norm;
                    pj["dry_db"] = p.dryDb;
                    pj["wet_db"] = p.wetDb;
                    bp.push_back (std::move (pj));
                }
                bj["positions"] = std::move (bp);
                blends.push_back (std::move (bj));
            }
            if (! blends.empty()) sj["blend"] = std::move (blends);
        }
        else if (st.kind == StageKind::Ir)
        {
            if (! st.irFiles.empty()) sj["files"] = st.irFiles;
        }
        else if (st.kind == StageKind::Eq)
        {
            if (! st.eq.model.empty()) sj["model"] = st.eq.model;
            if (st.eq.toneOnly)        sj["tone_only"]  = true;
            if (! st.eq.showCurve)     sj["show_curve"] = false;   // default true — write only the override
            if (! st.eq.defaults.empty())
            {
                nlohmann::ordered_json d = nlohmann::ordered_json::object();
                for (const auto& [k, v] : st.eq.defaults) d[k] = v;
                sj["defaults"] = std::move (d);
            }
            if (! st.eq.hidden.empty()) sj["hidden"] = st.eq.hidden;
        }
        chain.push_back (std::move (sj));
    }
    j["chain"] = std::move (chain);
    std::string out;
    detail::emit (out, j, "", 0, indent > 0 ? indent : 2, 100);
    out += '\n';
    return out;
}

} // namespace namz::rig

#endif // NAMZ_RIG_WRITE_H
