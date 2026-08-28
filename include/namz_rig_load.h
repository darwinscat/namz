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
    // Same discipline for numbers: a non-numeric value yields the default instead of throwing.
    inline double jnum (const nlohmann::json& j, const char* key, double dflt = 0.0)
    {
        if (auto it = j.find (key); it != j.end() && it->is_number()) return it->get<double>();
        return dflt;
    }
    inline int jint (const nlohmann::json& j, const char* key, int dflt = 0)
    {
        if (auto it = j.find (key); it != j.end() && it->is_number_integer()) return it->get<int>();
        return dflt;
    }
    // A pair of numbers under `key` — `range_db`, `hz_at`, `q_at`. Absent → false with `a`/`b` untouched;
    // present but not two numbers → also false, and the caller decides whether that spoils the band.
    inline bool jpair (const nlohmann::json& j, const char* key, double& a, double& b)
    {
        const auto it = j.find (key);
        if (it == j.end() || ! it->is_array() || it->size() != 2
            || ! (*it)[0].is_number() || ! (*it)[1].is_number()) return false;
        a = (*it)[0].get<double>();
        b = (*it)[1].get<double>();
        return true;
    }

    // One band of a `sections` knob. false = not a band this reader can build: unknown `type`, no
    // positive `hz`/`q`, or no `range_db` pair. A malformed OPTIONAL key (`hz_at`, `q_at`, a tilt's
    // `pivot`) is a stated quantity the reader cannot read — that spoils the band too, rather than
    // silently freezing something the producer said moves.
    inline bool readSection (const nlohmann::json& j, Section& out)
    {
        if (! sectionKindFrom (jstr (j, "type"), out.kind)) return false;
        out.hz = jnum (j, "hz");
        out.q  = jnum (j, "q");
        if (! (out.hz > 0.0) || ! (out.q > 0.0)) return false;
        if (! jpair (j, "range_db", out.dbAtMin, out.dbAtMax)) return false;
        // Only a tilt hinges. On any other kind the key is not read — not "read and ignored", and not
        // "read and refused": a bell with a stray `pivot` is a bell.
        if (out.kind == SectionKind::Tilt)
            if (auto p = j.find ("pivot"); p != j.end())
            {
                if (! p->is_number()) return false;
                out.pivot = p->get<double>();          // stored as written; the formula clamps when applied
            }
        // The pair at the stops: strictly positive, because a 0 is not a frequency and not a Q. A side
        // equal to the reference value is a side that does not move, which the model spells as 0 — so it
        // is normalised to 0 here, and what the writer spelled out reads back as what it was given.
        if (j.contains ("hz_at"))
        {
            if (! jpair (j, "hz_at", out.hzAtMin, out.hzAtMax)
                || ! (out.hzAtMin > 0.0) || ! (out.hzAtMax > 0.0)) return false;
            if (out.hzAtMin == out.hz) out.hzAtMin = 0.0;
            if (out.hzAtMax == out.hz) out.hzAtMax = 0.0;
        }
        if (j.contains ("q_at"))
        {
            if (! jpair (j, "q_at", out.qAtMin, out.qAtMax)
                || ! (out.qAtMin > 0.0) || ! (out.qAtMax > 0.0)) return false;
            if (out.qAtMin == out.q) out.qAtMin = 0.0;
            if (out.qAtMax == out.q) out.qAtMax = 0.0;
        }
        return true;
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
    // THE VERSION GATE, which was written and never read. `schema` is bumped only for a change a reader
    // cannot survive, so meeting a higher one means this reader is not equipped — and the honest answer
    // is to refuse, not to load whatever it recognises and silently drop the rest. Loading a schema-2
    // pack into a schema-1 reader is exactly the "everything parsed, nothing complained, the result was
    // confidently wrong" failure the format's removal policy exists to prevent, and until this was
    // checked the field was decoration.
    // …and a `schema` that is not an integer is refused too: `jint` used to answer 1 for `4.0` or `"4"`,
    // which let a pack this reader cannot judge through the gate as the oldest vintage there is.
    if (auto s = j.find ("schema"); s != j.end())
        if (! s->is_number_integer() || s->get<long long>() > kRigSchema) return rig;

    rig.rigId     = detail::jstr (j, "rig_id");
    rig.familyId  = detail::jstr (j, "family_id");
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
        st.toneType = detail::jstr (sj, "tone_type");
        st.voicing  = detail::jstr (sj, "voicing");
        st.circuit  = detail::jstr (sj, "circuit");

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
                    if (auto l = cj.find ("labels"); l != cj.end() && l->is_array())
                        for (const auto& lv : *l)
                            if (lv.is_string()) ctl.labels.push_back (lv.get<std::string>());
                    ctl.sweep = detail::jint (cj, "sweep");   // dial rotation; absent → not a dial
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
                    if (auto g = fj.find ("input_db"); g != fj.end() && g->is_number())
                        fe.inputDb = g->get<double>();
                    // An integer, or nothing: a lag of 2.5 samples is not a reading this format speaks.
                    if (auto l = fj.find ("lag_samples"); l != fj.end() && l->is_number_integer())
                        fe.lagSamples = l->get<int>();
                    if (! fe.id.empty()) st.device.files.push_back (std::move (fe));
                }

            // `tone` — linear knobs shipped as DSP instead of as model axes. Deliberately NOT in
            // `controls`: no file carries them in its settings, so a player that put them on the
            // selection dial would build a knob resolve() can never satisfy.
            if (auto m = sj.find ("tone"); m != sj.end() && m->is_array())
                for (const auto& mj : *m)
                {
                    if (! mj.is_object()) continue;
                    // ONE FORM PER KNOB. Both `positions` and `sections` on one knob is not a knob with a
                    // spare description, it is an invalid manifest — refused whole, like a schema this
                    // reader does not speak, and for the same reason: whichever form a reader picked, a
                    // different reader could pick the other, and the same pack would be two products.
                    if (mj.contains ("positions") && mj.contains ("sections"))
                    {
                        if (ok) *ok = false;
                        return Rig {};
                    }
                    Tone me;
                    me.name      = detail::jstr (mj, "name");
                    me.sweep     = detail::jint (mj, "sweep");
                    me.placement = detail::jstr (mj, "placement", "post");
                    me.reference    = detail::jstr (mj, "reference");
                    me.defaultValue = detail::jstr (mj, "default");
                    if (auto op = mj.find ("operating_point"); op != mj.end() && op->is_object())
                        for (auto it = op->begin(); it != op->end(); ++it)
                        {
                            if (it.value().is_string())            me.operatingPoint[it.key()] = it.value().get<std::string>();
                            else if (! it.value().is_structured()) me.operatingPoint[it.key()] = it.value().dump();
                        }
                    if (auto t = mj.find ("trusted"); t != mj.end() && t->is_object())
                    {
                        me.trusted.loHz    = detail::jnum (*t, "lo_hz");
                        me.trusted.hiHz    = detail::jnum (*t, "hi_hz");
                        me.trusted.loIndex = detail::jint (*t, "lo_index");
                        me.trusted.hiIndex = detail::jint (*t, "hi_index");
                        me.trusted.spanDb = detail::jnum (*t, "span_db");
                        me.trusted.levels = detail::jint (*t, "levels");
                    }
                    if (auto g = mj.find ("grid"); g != mj.end() && g->is_object())
                    {
                        me.grid.fLo    = detail::jnum (*g, "f_lo", 20.0);
                        me.grid.fHi    = detail::jnum (*g, "f_hi", 20000.0);
                        me.grid.points = detail::jint (*g, "points");
                    }
                    // `positions`: a position IS its curve — `db`, exactly `grid.points` numbers. One with
                    // no curve, or a curve of the wrong length (a non-number in it counts as a hole), is
                    // unusable and SKIPPED, not padded and not truncated; a knob with no grid, or with
                    // no position left after that, is dropped. NAMZ-FORMAT.md has said so since 2.0.0;
                    // the loader let all of it through, and the reader downstream was left to discover a
                    // short curve by indexing past it.
                    if (auto ps = mj.find ("positions"); ps != mj.end() && ps->is_array() && me.grid.points > 0)
                        for (const auto& pj : *ps)
                        {
                            if (! pj.is_object()) continue;
                            TonePosition p;
                            p.value      = detail::jstr (pj, "value");
                            p.label      = detail::jstr (pj, "label");
                            p.norm       = detail::jnum (pj, "norm");
                            p.levelDb    = detail::jnum (pj, "level_db");
                            if (auto db = pj.find ("db"); db != pj.end() && db->is_array())
                                for (const auto& v : *db)
                                    if (v.is_number()) p.db.push_back (v.get<double>());
                            if (! p.value.empty() && (int) p.db.size() == me.grid.points)
                                me.positions.push_back (std::move (p));
                        }
                    // `sections`: every band must be readable, or the KNOB is unusable — a band a reader
                    // cannot build (a `type` it does not know, no frequency, no `range_db`) is not skipped
                    // on its own, because the bands it does know would then play a different pedal. The
                    // knob is dropped and the reference position plays, which is what dropping a tone
                    // knob has always meant. The travel law runs on rotation, t = f(reference / sweep):
                    // a switch (no sweep), or a reference that is not a position on that dial, leaves it
                    // nothing to run on, so such a knob cannot carry sections either.
                    bool usable = true;
                    if (auto ss = mj.find ("sections"); ss != mj.end() && ss->is_array())
                        for (const auto& sx : *ss)
                        {
                            Section sc;
                            if (! sx.is_object() || ! detail::readSection (sx, sc)) { usable = false; break; }
                            me.sections.push_back (sc);
                        }
                    if (! me.sections.empty())
                    {
                        const int ref = detail::degrees (me.reference);
                        if (me.sweep <= 0 || ref < 0 || ref > me.sweep) usable = false;
                    }
                    if (usable && ! me.name.empty() && (me.positions.empty() != me.sections.empty()))
                        st.tone.push_back (std::move (me));
                }

            // `blend` — a dry/wet mix knob. Same reason as `tone` for staying out of `controls`, and
            // the same reference invariant: the models were captured at the full-WET end.
            if (auto b = sj.find ("blend"); b != sj.end() && b->is_array())
                for (const auto& bj : *b)
                {
                    if (! bj.is_object()) continue;
                    Blend bl;
                    bl.name      = detail::jstr (bj, "name");
                    bl.sweep     = detail::jint (bj, "sweep");
                    bl.reference = detail::jstr (bj, "reference");
                    bl.dryEnd       = detail::jstr (bj, "dry_end");
                    bl.defaultValue = detail::jstr (bj, "default");
                    bl.law           = detail::jstr (bj, "law", "linear");
                    bl.gainsMeasured = detail::jbool (bj, "gains_measured", false);
                    bl.polarity  = detail::jint (bj, "polarity", 1) < 0 ? -1 : 1;
                    if (auto op = bj.find ("operating_point"); op != bj.end() && op->is_object())
                        for (auto it = op->begin(); it != op->end(); ++it)
                        {
                            if (it.value().is_string())            bl.operatingPoint[it.key()] = it.value().get<std::string>();
                            else if (! it.value().is_structured()) bl.operatingPoint[it.key()] = it.value().dump();
                        }
                    if (auto t = bj.find ("trusted"); t != bj.end() && t->is_object())
                    {
                        bl.trusted.loHz    = detail::jnum (*t, "lo_hz");
                        bl.trusted.hiHz    = detail::jnum (*t, "hi_hz");
                        bl.trusted.loIndex = detail::jint (*t, "lo_index");
                        bl.trusted.hiIndex = detail::jint (*t, "hi_index");
                        bl.trusted.spanDb = detail::jnum (*t, "span_db");
                        bl.trusted.levels = detail::jint (*t, "levels");
                    }
                    if (auto g = bj.find ("grid"); g != bj.end() && g->is_object())
                    {
                        bl.grid.fLo    = detail::jnum (*g, "f_lo", 20.0);
                        bl.grid.fHi    = detail::jnum (*g, "f_hi", 20000.0);
                        bl.grid.points = detail::jint (*g, "points");
                    }
                    bl.dryLevelDb = detail::jnum (bj, "dry_level_db");
                    if (auto d = bj.find ("dry"); d != bj.end() && d->is_array())
                        for (const auto& v : *d)
                            if (v.is_number()) bl.dryDb.push_back (v.get<double>());
                    if (auto ps = bj.find ("positions"); ps != bj.end() && ps->is_array())
                        for (const auto& pj : *ps)
                        {
                            if (! pj.is_object()) continue;
                            BlendPosition p;
                            p.value = detail::jstr (pj, "value");
                            p.label = detail::jstr (pj, "label");
                            p.norm  = detail::jnum (pj, "norm");
                            p.dryDb = detail::jnum (pj, "dry_db", -120.0);
                            p.wetDb = detail::jnum (pj, "wet_db");
                            if (! p.value.empty()) bl.positions.push_back (std::move (p));
                        }
                    if (! bl.name.empty() && ! bl.positions.empty()) st.blend.push_back (std::move (bl));
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
