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

// rig.json text — the pack's source of truth (loadRigManifest is the exact inverse for every field
// the model carries). Empty strings are omitted; an Unknown stage writes its preserved rawKind so
// a pack passing through an old tool never loses stages it didn't understand.
inline std::string writeManifest (const Rig& rig, int indent = 2)
{
    nlohmann::json j;
    j["format"] = "orbitrig";
    j["schema"] = 1;
    if (! rig.rigId.empty())     j["rig_id"]     = rig.rigId;
    if (! rig.familyId.empty())  j["family_id"]  = rig.familyId;
    if (! rig.name.empty())      j["name"]       = rig.name;
    if (! rig.modeledBy.empty()) j["modeled_by"] = rig.modeledBy;

    auto chain = nlohmann::json::array();
    for (const auto& st : rig.chain)
    {
        nlohmann::json sj;
        const auto kind = st.kind != StageKind::Unknown ? std::string (stageKindToString (st.kind))
                                                        : st.rawKind;
        if (kind.empty()) continue;   // a stage with no kind at all cannot be represented
        sj["kind"] = kind;
        if (! st.slot.empty()) sj["slot"] = st.slot;
        if (! st.make.empty() || ! st.model.empty() || ! st.gearType.empty())
        {
            nlohmann::json g;
            if (! st.make.empty())     g["make"]  = st.make;
            if (! st.model.empty())    g["model"] = st.model;
            if (! st.gearType.empty()) g["type"]  = st.gearType;
            sj["gear"] = std::move (g);
        }
        if (! st.toneType.empty()) sj["tone_type"] = st.toneType;
        if (! st.voicing.empty())  sj["voicing"]   = st.voicing;
        if (! st.circuit.empty())  sj["circuit"]   = st.circuit;

        if (st.kind == StageKind::Nam)
        {
            auto controls = nlohmann::json::array();
            for (const auto& c : st.device.controls)
            {
                nlohmann::json cj;
                cj["name"]   = c.name;
                cj["role"]   = roleToString (c.role);
                if (c.sweep > 0) cj["sweep"] = c.sweep;
                cj["values"] = c.values;
                if (! c.labels.empty()) cj["labels"] = c.labels;
                controls.push_back (std::move (cj));
            }
            if (! controls.empty()) sj["controls"] = std::move (controls);

            auto files = nlohmann::json::array();
            for (const auto& fe : st.device.files)
            {
                nlohmann::json fj;
                fj["file"] = fe.id;
                nlohmann::json s = nlohmann::json::object();
                for (const auto& [k, v] : fe.settings) s[k] = v;
                fj["settings"] = std::move (s);
                if (fe.inputDb < -0.0005 || fe.inputDb > 0.0005)
                    fj["input_db"] = std::round (fe.inputDb * 100.0) / 100.0;
                files.push_back (std::move (fj));
            }
            if (! files.empty()) sj["files"] = std::move (files);

            // The measured (linear) knobs — see namz_rig.h. Client-ready first: `norm` + a fit in
            // PHYSICAL units (Hz / dB), which survives any sample rate, with the raw `db` curve as
            // optional provenance a player may upgrade to later.
            auto measured = nlohmann::json::array();
            for (const auto& me : st.measured)
            {
                if (me.name.empty() || me.positions.empty()) continue;
                nlohmann::json mj;
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
                    nlohmann::json op = nlohmann::json::object();
                    for (const auto& [k, v] : me.operatingPoint) op[k] = v;
                    mj["operating_point"] = std::move (op);
                }
                // The grid and the curves travel together or not at all: a curve without the grid it
                // was sampled on cannot be applied to anything.
                if (me.grid.points > 0)
                {
                    nlohmann::json g;
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
                    nlohmann::json t;
                    t["lo_hz"]    = me.trusted.loHz;
                    t["hi_hz"]    = me.trusted.hiHz;
                    t["lo_index"] = me.trusted.loIndex;
                    t["hi_index"] = me.trusted.hiIndex;
                    t["span_db"]  = me.trusted.spanDb;
                    t["levels"]   = me.trusted.levels;
                    mj["trusted"] = std::move (t);
                }
                auto positions = nlohmann::json::array();
                for (const auto& p : me.positions)
                {
                    nlohmann::json pj;
                    pj["value"]    = p.value;
                    if (! p.label.empty()) pj["label"] = p.label;
                    pj["norm"]     = p.norm;
                    pj["level_db"] = p.levelDb;
                    pj["db"]       = p.db;
                    positions.push_back (std::move (pj));
                }
                mj["positions"] = std::move (positions);
                measured.push_back (std::move (mj));
            }
            if (! measured.empty()) sj["measured"] = std::move (measured);

            auto blends = nlohmann::json::array();
            for (const auto& bl : st.blend)
            {
                if (bl.name.empty()) continue;
                nlohmann::json bj;
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
                    nlohmann::json op = nlohmann::json::object();
                    for (const auto& [k, v] : bl.operatingPoint) op[k] = v;
                    bj["operating_point"] = std::move (op);
                }
                if (bl.grid.points > 0)
                {
                    nlohmann::json g;
                    g["f_lo"]   = bl.grid.fLo;
                    g["f_hi"]   = bl.grid.fHi;
                    g["points"] = bl.grid.points;
                    bj["grid"] = std::move (g);
                }
                if (bl.trusted.levels >= 2)   // …and the same for a blend
                {
                    nlohmann::json t;
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
                auto bp = nlohmann::json::array();
                for (const auto& p : bl.positions)
                {
                    nlohmann::json pj;
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
                nlohmann::json d = nlohmann::json::object();
                for (const auto& [k, v] : st.eq.defaults) d[k] = v;
                sj["defaults"] = std::move (d);
            }
            if (! st.eq.hidden.empty()) sj["hidden"] = st.eq.hidden;
        }
        chain.push_back (std::move (sj));
    }
    j["chain"] = std::move (chain);
    return j.dump (indent) + "\n";
}

} // namespace namz::rig

#endif // NAMZ_RIG_WRITE_H
