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

    if (const auto spec = buildControlsSpec (stage.device.controls); ! spec.empty())
        m["controls"] = spec;
    for (const auto& [k, v] : fileSettings)
        m["settings." + k] = v;

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

        if (st.kind == StageKind::Nam)
        {
            auto controls = nlohmann::json::array();
            for (const auto& c : st.device.controls)
            {
                nlohmann::json cj;
                cj["name"]   = c.name;
                cj["role"]   = roleToString (c.role);
                cj["values"] = c.values;
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
                files.push_back (std::move (fj));
            }
            if (! files.empty()) sj["files"] = std::move (files);
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
