// SPDX-License-Identifier: MIT
// namz_rig_load — build a namz::rig::Rig from an .orbitrig pack. Two sources, manifest-first:
//   • loadRigManifest(rig.json text)         — the pack's source of truth (priority)
//   • loadRigFromFiles(per-file .namz meta)  — standalone / manifest-less fallback via buildDevices
//   • loadRig(manifest text, file meta)      — manifest when valid, else the fallback
// Split from namz_rig.h because this one needs a JSON parser (nlohmann, the same dep as namz.h),
// while the core selector (namz_rig.h) stays std-only. A stage whose `kind` is unknown is kept in
// the chain as StageKind::Unknown so the player can skip it without the pack failing to load.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.

#ifndef NAMZ_RIG_LOAD_H
#define NAMZ_RIG_LOAD_H

#include "namz_rig.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace namz::rig
{

namespace detail
{
    inline std::string jstr (const nlohmann::json& j, const char* key, const std::string& dflt = {})
    {
        if (auto it = j.find (key); it != j.end() && it->is_string()) return it->get<std::string>();
        return dflt;
    }
    // Type-safe bool: a non-boolean value (string "true", number 1) returns the default — never
    // throws (nlohmann's value() would throw type_error.302 and take down the whole load).
    inline bool jbool (const nlohmann::json& j, const char* key, bool dflt)
    {
        if (auto it = j.find (key); it != j.end() && it->is_boolean()) return it->get<bool>();
        return dflt;
    }
} // namespace detail

// Parse rig.json. `ok` (when given) reports whether the text WAS a valid orbitrig manifest (valid
// JSON object, format=="orbitrig", "chain" is an array) — distinct from "chain has stages", so a
// deliberately empty pack (chain:[]) is still the source of truth, not a fallback trigger. Returns
// a Rig with an empty chain when the text is not a valid manifest.
inline Rig loadRigManifest (const std::string& manifestText, bool* ok = nullptr)
{
    if (ok) *ok = false;
    Rig rig;
    nlohmann::json j = nlohmann::json::parse (manifestText, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || ! j.is_object()) return rig;
    if (detail::jstr (j, "format") != "orbitrig") return rig;

    rig.rigId     = detail::jstr (j, "rig_id");
    rig.name      = detail::jstr (j, "name");
    rig.modeledBy = detail::jstr (j, "modeled_by");

    const auto chainIt = j.find ("chain");
    if (chainIt == j.end() || ! chainIt->is_array()) return rig;   // no runnable chain
    if (ok) *ok = true;                                            // a valid manifest (chain may be empty)

    for (const auto& sj : *chainIt)
    {
        if (! sj.is_object()) continue;
        Stage st;
        st.rawKind  = detail::jstr (sj, "kind");
        st.kind     = stageKindFrom (st.rawKind);
        st.slot     = detail::jstr (sj, "slot");
        if (auto g = sj.find ("gear"); g != sj.end() && g->is_object())
        {
            st.make     = detail::jstr (*g, "make");
            st.model    = detail::jstr (*g, "model");
            st.gearType = detail::jstr (*g, "type");
        }

        if (st.kind == StageKind::Nam)
        {
            st.device.rigId = rig.rigId;
            st.device.slot  = st.slot;
            st.device.family = ! st.model.empty() ? st.model : rig.name;
            if (auto c = sj.find ("controls"); c != sj.end() && c->is_array())
                for (const auto& cj : *c)
                {
                    if (! cj.is_object()) continue;
                    Control ctl;
                    ctl.name = detail::jstr (cj, "name");
                    ctl.role = roleFromString (detail::jstr (cj, "role", "generic"));
                    if (auto v = cj.find ("values"); v != cj.end() && v->is_array())
                        for (const auto& vv : *v)
                        {
                            // The format types all-digit values to int (NAMZ-FORMAT §metadata), so a
                            // value may arrive as a JSON number/bool — stringify scalars, skip only
                            // structured values (mirrors the EQ-defaults handling below).
                            if (vv.is_string())            ctl.values.push_back (vv.get<std::string>());
                            else if (! vv.is_structured()) ctl.values.push_back (vv.dump());
                        }
                    if (! ctl.name.empty() && ! ctl.values.empty()) st.device.controls.push_back (std::move (ctl));
                }
            if (auto f = sj.find ("files"); f != sj.end() && f->is_array())
                for (const auto& fj : *f)
                {
                    if (! fj.is_object()) continue;
                    FileEntry fe;
                    fe.id = detail::jstr (fj, "file");
                    if (auto s = fj.find ("settings"); s != fj.end() && s->is_object())
                        for (auto it = s->begin(); it != s->end(); ++it)
                        {
                            // int-typed knob positions arrive as JSON numbers — stringify scalars,
                            // skip structured (else the whole stage becomes unselectable).
                            if (it.value().is_string())            fe.settings[it.key()] = it.value().get<std::string>();
                            else if (! it.value().is_structured()) fe.settings[it.key()] = it.value().dump();
                        }
                    if (! fe.id.empty()) st.device.files.push_back (std::move (fe));
                }
        }
        else if (st.kind == StageKind::Ir)
        {
            if (auto f = sj.find ("files"); f != sj.end() && f->is_array())
                for (const auto& fj : *f)
                    if (fj.is_string()) st.irFiles.push_back (fj.get<std::string>());
        }
        else if (st.kind == StageKind::Eq)
        {
            st.eq.model    = detail::jstr (sj, "model");
            st.eq.toneOnly = detail::jbool (sj, "tone_only", false);
            st.eq.showCurve = detail::jbool (sj, "show_curve", true);
            if (auto d = sj.find ("defaults"); d != sj.end() && d->is_object())
                for (auto it = d->begin(); it != d->end(); ++it)
                {
                    // A knob default is a SCALAR (string/number/bool). Objects/arrays are not valid
                    // defaults — skip them (dump()ing a hostile deeply-nested value overflows the
                    // stack). Scalars dump without recursion.
                    if (it.value().is_structured()) continue;
                    st.eq.defaults[it.key()] = it.value().is_string() ? it.value().get<std::string>()
                                                                      : it.value().dump();
                }
            if (auto h = sj.find ("hidden"); h != sj.end() && h->is_array())
                for (const auto& hv : *h)
                    if (hv.is_string()) st.eq.hidden.push_back (hv.get<std::string>());
        }
        // Unknown kind: kept as-is (rawKind preserved); firstKnown()/players skip it.
        rig.chain.push_back (std::move (st));
    }
    return rig;
}

// Standalone / manifest-less: wrap buildDevices() output as one Nam stage per device.
inline Rig loadRigFromFiles (const std::vector<FileMeta>& files)
{
    Rig rig;
    for (auto& d : buildDevices (files))
    {
        Stage st;
        st.kind  = StageKind::Nam;
        st.rawKind = "nam";
        st.slot  = d.slot;
        st.model = d.family;
        if (rig.rigId.empty()) rig.rigId = d.rigId;
        st.device = std::move (d);
        rig.chain.push_back (std::move (st));
    }
    if (rig.name.empty() && ! rig.chain.empty()) rig.name = rig.chain.front().model;
    return rig;
}

// Manifest first (its chain wins — spec decision A); fall back to file metas ONLY when the manifest
// is missing/invalid/wrong-format (NOT when it is a valid manifest with a deliberately empty chain).
inline Rig loadRig (const std::string& manifestText, const std::vector<FileMeta>& files)
{
    bool validManifest = false;
    Rig rig = loadRigManifest (manifestText, &validManifest);
    if (validManifest) return rig;
    return loadRigFromFiles (files);
}

} // namespace namz::rig

#endif // NAMZ_RIG_LOAD_H
