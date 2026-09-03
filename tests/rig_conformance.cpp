// SPDX-License-Identifier: MIT
// The reference consumer of the RIG conformance fixtures — the executable form of the .orbitrig
// contract between a capture tool (writer) and a player (reader). Three duties per fixture:
// READ the golden pack into expected.json's device, SELECT per the expectation table (the fallback
// policy, not just the happy path), and WRITE the model back byte-exact (writeManifest == rig.json,
// stampMeta == each file's header). See conformance/rig/README.md for what consumers must run.
//
// This binary is ALSO the regeneration recipe: run it with NAMZ_REGEN=1 and it writes the packs from
// expected.json instead of comparing against them. Regeneration on an unchanged fixture is a no-op —
// that is exactly what the WRITE duty asserts.
//
// Fixtures:
//   conformance/rig/          a sparse channel×gain matrix — pins the SELECTION policy
//   conformance/rig-measured/ one gain dial + two TONE (linear) knobs, one per form — the minimal
//                             shape a player must handle: pick a model by the dial, apply a knob
//                             shipped as a measured ladder (`positions`) and one shipped as bands
//                             (`sections`) as DSP
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.

#define NAMZ_IMPLEMENTATION
#include "namz.h"
#include "namz_rig.h"
#include "namz_rig_load.h"
#include "namz_rig_write.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace namz::rig;

#ifndef NAMZ_CONFORMANCE_DIR
#error "NAMZ_CONFORMANCE_DIR must be defined (path to the conformance/ directory)"
#endif

static int failures = 0;
static void ok (bool cond, const char* what)
{
    if (! cond) { ++failures; std::printf ("    FAIL: %s\n", what); }
}

static std::string readFile (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    return { std::istreambuf_iterator<char> (f), std::istreambuf_iterator<char>() };
}

static void writeFile (const std::string& path, const void* data, std::size_t size)
{
    std::ofstream f (path, std::ios::binary);
    f.write (static_cast<const char*> (data), (std::streamsize) size);
}

// expected.json → the model. The ONE place a fixture becomes a Rig: the WRITE duty then proves that
// writing this model reproduces the committed bytes, so the fixture and the library cannot drift.
static Rig buildModel (const json& expected)
{
    Rig rig;
    rig.rigId     = expected["rig"]["rig_id"].get<std::string>();
    rig.name      = expected["rig"]["name"].get<std::string>();
    rig.modeledBy = expected["rig"]["modeled_by"].get<std::string>();

    const auto& sx = expected["stage"];
    Stage st;
    st.kind = StageKind::Nam; st.rawKind = "nam";
    st.slot     = sx["slot"].get<std::string>();
    st.make     = sx["gear"]["make"].get<std::string>();
    st.model    = sx["gear"]["model"].get<std::string>();
    st.gearType = sx["gear"]["type"].get<std::string>();
    if (auto t = sx.find ("tone_type"); t != sx.end()) st.toneType = t->get<std::string>();
    st.device.rigId = rig.rigId; st.device.slot = st.slot; st.device.family = st.model;
    st.device.controls = parseControlsSpec (sx["controls"].get<std::string>());
    if (auto sw = sx.find ("sweep"); sw != sx.end())
        for (auto& c : st.device.controls)
            if (auto it = sw->find (c.name); it != sw->end()) c.sweep = it->get<int>();

    if (auto ms = sx.find ("tone"); ms != sx.end())
        for (const auto& mx : *ms)
        {
            Tone me;
            me.name      = mx["name"].get<std::string>();
            me.sweep     = mx.contains ("sweep") ? mx["sweep"].get<int>() : 0;
            me.placement = mx["placement"].get<std::string>();
            me.reference = mx["reference"].get<std::string>();
            if (auto d = mx.find ("default"); d != mx.end()) me.defaultValue = d->get<std::string>();
            // …and provenance is optional too: a knob may hold nothing while sweeping. Read
            // unconditionally off a const json, a missing key here is undefined behaviour.
            if (auto op = mx.find ("operating_point"); op != mx.end())
                for (const auto& [k, v] : op->items()) me.operatingPoint[k] = v.get<std::string>();
            if (auto g = mx.find ("grid"); g != mx.end())      // positions only
            {
                me.grid.fLo    = (*g)["f_lo"].get<double>();
                me.grid.fHi    = (*g)["f_hi"].get<double>();
                me.grid.points = (*g)["points"].get<int>();
            }
            if (auto t = mx.find ("trusted"); t != mx.end())
            {
                me.trusted.loHz    = (*t)["lo_hz"].get<double>();
                me.trusted.hiHz    = (*t)["hi_hz"].get<double>();
                // The fixture has always stated these two, and the pack always carried 0/0: this recipe
                // never copied them, and checkTone never looked. The blend block below did both.
                me.trusted.loIndex = (*t)["lo_index"].get<int>();
                me.trusted.hiIndex = (*t)["hi_index"].get<int>();
                me.trusted.spanDb  = (*t)["span_db"].get<double>();
                me.trusted.levels  = (*t)["levels"].get<int>();
            }
            // ONE form per knob — the fixture states one, and the model built from it carries one.
            if (auto ps = mx.find ("positions"); ps != mx.end())
                for (const auto& px : *ps)
                {
                    TonePosition p;
                    p.value = px["value"].get<std::string>();
                    // A SWITCH HAS NO ANGLE. Read unconditionally, a missing `norm` here was an assert in
                    // a debug build and undefined behaviour in a release one — the harness could not have
                    // held a switch fixture at all, which is why there was never one to hold.
                    if (px.contains ("norm")) p.norm = px["norm"].get<double>();
                    if (auto sx2 = px.find ("sections"); sx2 != px.end())
                        for (const auto& b : *sx2)
                        {
                            PositionSection ps2;
                            ok (sectionKindFrom (b["type"].get<std::string>(), ps2.kind), "fixture position band type is known");
                            ps2.hz     = b["hz"].get<double>();
                            ps2.q      = b["q"].get<double>();
                            ps2.gainDb = b["gain_db"].get<double>();
                            if (b.contains ("pivot")) ps2.pivot = b["pivot"].get<double>();
                            p.sections.push_back (ps2);
                        }
                    else
                    {
                        p.levelDb = px["level_db"].get<double>();   // same story as the indices above
                        p.db      = px["db"].get<std::vector<double>>();
                    }
                    me.positions.push_back (std::move (p));
                }
            if (auto ss = mx.find ("sections"); ss != mx.end())
                for (const auto& x : *ss)
                {
                    Section sc;
                    ok (sectionKindFrom (x["type"].get<std::string>(), sc.kind), "fixture section type is known");
                    sc.hz      = x["hz"].get<double>();
                    sc.q       = x["q"].get<double>();
                    sc.dbAtMin = x["range_db"][0].get<double>();
                    sc.dbAtMax = x["range_db"][1].get<double>();
                    if (auto p = x.find ("pivot"); p != x.end()) sc.pivot = p->get<double>();
                    if (auto h = x.find ("hz_at"); h != x.end()) { sc.hzAtMin = (*h)[0].get<double>(); sc.hzAtMax = (*h)[1].get<double>(); }
                    if (auto q = x.find ("q_at");  q != x.end()) { sc.qAtMin  = (*q)[0].get<double>(); sc.qAtMax  = (*q)[1].get<double>(); }
                    me.sections.push_back (sc);
                }
            st.tone.push_back (std::move (me));
        }

    // The blend block, which had no fixture at all until now — the newest and most fragile part of the
    // format, and the one where a reader's mistakes are silent: a wrong polarity or a dB-interpolated
    // gain produces audio, just not the pedal's.
    if (auto bs = sx.find ("blend"); bs != sx.end())
        for (const auto& bx : *bs)
        {
            Blend bl;
            bl.name          = bx["name"].get<std::string>();
            bl.sweep         = bx.contains ("sweep") ? bx["sweep"].get<int>() : 0;
            bl.reference     = bx["reference"].get<std::string>();
            bl.dryEnd        = bx["dry_end"].get<std::string>();
            bl.defaultValue  = bx["default"].get<std::string>();
            bl.law           = bx["law"].get<std::string>();
            bl.gainsMeasured = bx["gains_measured"].get<bool>();
            bl.polarity      = bx["polarity"].get<int>();
            bl.grid.fLo      = bx["grid"]["f_lo"].get<double>();
            bl.grid.fHi      = bx["grid"]["f_hi"].get<double>();
            bl.grid.points   = bx["grid"]["points"].get<int>();
            bl.trusted.loHz    = bx["trusted"]["lo_hz"].get<double>();
            bl.trusted.hiHz    = bx["trusted"]["hi_hz"].get<double>();
            bl.trusted.loIndex = bx["trusted"]["lo_index"].get<int>();
            bl.trusted.hiIndex = bx["trusted"]["hi_index"].get<int>();
            bl.trusted.spanDb  = bx["trusted"]["span_db"].get<double>();
            bl.trusted.levels  = bx["trusted"]["levels"].get<int>();
            bl.dryDb           = bx["dry"].get<std::vector<double>>();
            bl.dryLevelDb      = bx["dry_level_db"].get<double>();
            for (const auto& px : bx["positions"])
            {
                BlendPosition bp;
                bp.value = px["value"].get<std::string>();
                bp.norm  = px["norm"].get<double>();
                bp.dryDb = px["dry_db"].get<double>();
                bp.wetDb = px["wet_db"].get<double>();
                bl.positions.push_back (std::move (bp));
            }
            st.blend.push_back (std::move (bl));
        }

    for (const auto& [name, settings] : sx["files"].items())
    {
        FileEntry fe;
        fe.id = name;
        for (const auto& [k, v] : settings.items()) fe.settings[k] = v.get<std::string>();
        st.device.files.push_back (std::move (fe));
    }
    rig.chain = { std::move (st) };
    return rig;
}

// The tone block a player reads must equal the one the fixture declares — including the curve,
// which is the only thing that lets a better filter design ship later without re-measuring, and
// including every number of a band, because a band is applied from those numbers and nothing else.
static void checkTone (const Stage& st, const json& sx, const char* via)
{
    char what[160];
    auto tag = [&what, via] (const char* w) { std::snprintf (what, sizeof (what), "%s (%s)", w, via); return what; };
    const auto ms = sx.find ("tone");
    const std::size_t want = ms == sx.end() ? 0 : ms->size();
    ok (st.tone.size() == want, tag ("tone count"));
    if (st.tone.size() != want) return;
    for (std::size_t i = 0; i < want; ++i)
    {
        const auto& me = st.tone[i];
        const auto& mx = (*ms)[i];
        ok (me.name == mx["name"].get<std::string>()
            && me.sweep == (mx.contains ("sweep") ? mx["sweep"].get<int>() : 0)
            && me.placement == mx["placement"].get<std::string>()
            && me.reference == mx["reference"].get<std::string>(), tag ("tone identity"));
        // Where the knob starts. A round trip that drops this is not detectably wrong — the field is
        // optional and its absence means "start at the reference" — so only a fixture that sets it can
        // catch a writer that forgets it. One did forget.
        ok (me.defaultValue == (mx.find ("default") != mx.end() ? mx["default"].get<std::string>() : std::string{}),
            tag ("tone default position survives the round trip"));
        if (auto t = mx.find ("trusted"); t != mx.end())
            ok (me.trusted.loHz == (*t)["lo_hz"].get<double>()
                && me.trusted.hiHz == (*t)["hi_hz"].get<double>()
                && me.trusted.loIndex == (*t)["lo_index"].get<int>()
                && me.trusted.hiIndex == (*t)["hi_index"].get<int>()
                && me.trusted.spanDb == (*t)["span_db"].get<double>()
                && me.trusted.levels == (*t)["levels"].get<int>(),
                tag ("the band the curve may be applied in, in Hz and as grid indices"));
        // The knob is DSP, not an axis: it must never appear among the selectable controls.
        for (const auto& c : st.device.controls)
            ok (c.name != me.name, tag ("tone knob is not a selection control"));

        // ONE form per knob: whichever the fixture states, the other side of the model is empty.
        if (const auto ss = mx.find ("sections"); ss != mx.end())
        {
            ok (me.positions.empty() && me.grid.points == 0, tag ("a sections knob carries no ladder and no grid"));
            ok (me.sections.size() == ss->size(), tag ("section count"));
            if (me.sections.size() != ss->size()) continue;
            for (std::size_t k = 0; k < me.sections.size(); ++k)
            {
                const auto& got = me.sections[k];
                const auto& x   = (*ss)[k];
                ok (std::string (sectionKindToString (got.kind)) == x["type"].get<std::string>(), tag ("section type"));
                ok (got.hz == x["hz"].get<double>() && got.q == x["q"].get<double>(), tag ("section hz/q at the reference"));
                ok (got.dbAtMin == x["range_db"][0].get<double>() && got.dbAtMax == x["range_db"][1].get<double>(),
                    tag ("section range_db: signed, minus stop then plus stop"));
                // A tilt's hinge is fitted, not assumed — the fixture states 0.35 so that a reader which
                // silently substitutes the seesaw's 0.5 fails here.
                if (auto p = x.find ("pivot"); p != x.end())
                    ok (got.pivot == p->get<double>(), tag ("section pivot"));
                // The travel: a stated pair round-trips as stated; an absent one stays 0 = does not move.
                if (auto h = x.find ("hz_at"); h != x.end())
                    ok (got.hzAtMin == (*h)[0].get<double>() && got.hzAtMax == (*h)[1].get<double>(), tag ("section hz_at"));
                else
                    ok (got.hzAtMin == 0.0 && got.hzAtMax == 0.0, tag ("section frequency does not move"));
                if (auto q = x.find ("q_at"); q != x.end())
                    ok (got.qAtMin == (*q)[0].get<double>() && got.qAtMax == (*q)[1].get<double>(), tag ("section q_at"));
                else
                    ok (got.qAtMin == 0.0 && got.qAtMax == 0.0, tag ("section Q does not move"));
            }
            continue;
        }

        ok (me.sections.empty(), tag ("a positions knob carries no sections"));
        // A knob that ships bands per position ships no curve, so it has no grid to describe one.
        const bool bandsPerPosition = ! mx["positions"].empty() && mx["positions"][0].contains ("sections");
        if (! bandsPerPosition)
            ok (me.grid.points == mx["grid"]["points"].get<int>(), tag ("tone grid"));
        else
            ok (me.grid.points == 0, tag ("a bands-per-position knob carries no grid"));
        ok (me.positions.size() == mx["positions"].size(), tag ("tone position count"));
        if (me.positions.size() != mx["positions"].size()) continue;
        for (std::size_t p = 0; p < me.positions.size(); ++p)
        {
            const auto& got = me.positions[p];
            const auto& px  = mx["positions"][p];
            ok (got.value == px["value"].get<std::string>()
                && got.norm == (px.contains ("norm") ? px["norm"].get<double>() : 0.0),
                tag ("tone position identity"));
            if (auto sx2 = px.find ("sections"); sx2 != px.end())
            {
                ok (got.sections.size() == sx2->size() && got.db.empty(), tag ("position band count"));
                for (std::size_t b = 0; b < got.sections.size() && b < sx2->size(); ++b)
                    ok (got.sections[b].hz == (*sx2)[b]["hz"].get<double>()
                        && got.sections[b].q == (*sx2)[b]["q"].get<double>()
                        && got.sections[b].gainDb == (*sx2)[b]["gain_db"].get<double>(),
                        tag ("position band"));
            }
            else
            {
                ok (got.levelDb == px["level_db"].get<double>(), tag ("tone position level"));
                ok (got.db == px["db"].get<std::vector<double>>(), tag ("tone curve"));
            }
        }
    }

    if (auto bs = sx.find ("blend"); bs != sx.end())
    {
        ok (st.blend.size() == bs->size(), tag ("blend block count"));
        for (std::size_t i = 0; i < st.blend.size() && i < bs->size(); ++i)
        {
            const auto& bl = st.blend[i];
            const auto& bx = (*bs)[i];
            ok (bl.name == bx["name"].get<std::string>()
                && bl.reference == bx["reference"].get<std::string>()
                && bl.dryEnd == bx["dry_end"].get<std::string>()
                && bl.defaultValue == bx["default"].get<std::string>(),
                tag ("blend identity: both ends named, and where the knob starts"));
            // The two facts a reader cannot recover and cannot guess: how loud the dry path is against
            // the model, and which way round the two branches sum.
            ok (bl.dryLevelDb == bx["dry_level_db"].get<double>(), tag ("blend dry level"));
            ok (bl.polarity == bx["polarity"].get<int>(), tag ("blend polarity"));
            ok (bl.gainsMeasured == bx["gains_measured"].get<bool>(),
                tag ("blend gains: measured, or derived from the law"));
            ok (bl.dryDb == bx["dry"].get<std::vector<double>>(), tag ("blend dry curve"));
            ok (bl.trusted.loIndex == bx["trusted"]["lo_index"].get<int>()
                && bl.trusted.hiIndex == bx["trusted"]["hi_index"].get<int>(),
                tag ("blend trusted band, as grid indices"));
            ok (bl.positions.size() == bx["positions"].size(), tag ("blend position count"));
            for (std::size_t k = 0; k < bl.positions.size() && k < bx["positions"].size(); ++k)
            {
                const auto& got = bl.positions[k];
                const auto& px  = bx["positions"][k];
                ok (got.value == px["value"].get<std::string>() && got.norm == px["norm"].get<double>()
                    && got.dryDb == px["dry_db"].get<double>() && got.wetDb == px["wet_db"].get<double>(),
                    tag ("blend position: rotation and both gains"));
            }
            // Same rule as measured, and for the same reason: no file carries a blend in its settings.
            for (const auto& c : st.device.controls)
                ok (c.name != bl.name, tag ("blend knob is not a selection control"));
        }
    }
}

static void runFixture (const std::string& dir, const std::string& flatNam, bool regen)
{
    std::printf ("  fixture: %s\n", dir.c_str());
    const json expected = json::parse (readFile (dir + "/expected.json"));
    const Rig model     = buildModel (expected);
    const Stage& st     = model.chain.front();

    if (regen)
    {
        for (const auto& fe : st.device.files)
        {
            namz::PackOptions opts;
            opts.metadata = stampMeta (model, st, fe.settings);
            const auto packed = namz::pack (flatNam.data(), flatNam.size(), opts);
            writeFile (dir + "/pack/" + fe.id, packed.data(), packed.size());
        }
        const auto manifest = writeManifest (model);
        writeFile (dir + "/pack/rig.json", manifest.data(), manifest.size());
        std::printf ("    regenerated %zu models + rig.json\n", st.device.files.size());
        return;
    }

    const std::string goldenManifest = readFile (dir + "/pack/rig.json");
    ok (! goldenManifest.empty(), "golden rig.json present");

    // Per-file header metadata, read with the codec's cheap header reader — the player's path.
    std::vector<FileMeta> files;
    for (const auto& [name, settings] : expected["stage"]["files"].items())
    {
        const auto bytes = readFile (dir + "/pack/" + name);
        ok (! bytes.empty(), "golden model present");
        FileMeta f;
        f.id = name;
        f.filenameBase = name.substr (0, name.rfind ('.'));
        f.meta = namz::readMeta (bytes.data(), bytes.size());
        files.push_back (std::move (f));
    }

    // --- 1. READ: manifest-first, and the manifest-less fallback builds the SAME device ----------
    auto checkRig = [&expected] (const Rig& rig, const char* via)
    {
        char what[160];
        auto tag = [&what, via] (const char* w) { std::snprintf (what, sizeof (what), "%s (%s)", w, via); return what; };
        ok (rig.rigId     == expected["rig"]["rig_id"].get<std::string>()
            && rig.name      == expected["rig"]["name"].get<std::string>()
            && rig.modeledBy == expected["rig"]["modeled_by"].get<std::string>(), tag ("rig identity"));
        const auto* st = rig.firstKnown();
        ok (st != nullptr && st->kind == StageKind::Nam, tag ("one nam stage"));
        if (st == nullptr) return;
        ok (st->slot == expected["stage"]["slot"].get<std::string>(), tag ("slot"));
        ok (buildControlsSpec (st->device.controls) == expected["stage"]["controls"].get<std::string>(),
            tag ("controls spec"));
        if (auto sw = expected["stage"].find ("sweep"); sw != expected["stage"].end())
            for (const auto& c : st->device.controls)
                if (auto it = sw->find (c.name); it != sw->end())
                    ok (c.sweep == it->get<int>(), tag ("dial sweep"));
        ok (st->device.files.size() == expected["stage"]["files"].size(), tag ("file count"));
        for (const auto& fe : st->device.files)
        {
            Settings want;
            for (const auto& [k, v] : expected["stage"]["files"][fe.id].items())
                want[k] = v.get<std::string>();
            ok (fe.settings == want, tag ("file settings"));
        }
    };

    const Rig fromManifest = loadRig (goldenManifest, files);
    checkRig (fromManifest, "manifest");
    ok (fromManifest.firstKnown() != nullptr
        && fromManifest.firstKnown()->make     == expected["stage"]["gear"]["make"].get<std::string>()
        && fromManifest.firstKnown()->model    == expected["stage"]["gear"]["model"].get<std::string>()
        && fromManifest.firstKnown()->gearType == expected["stage"]["gear"]["type"].get<std::string>(),
        "gear caption (manifest)");
    checkTone (*fromManifest.firstKnown(), expected["stage"], "manifest");

    Rig fromHeaders = loadRigFromFiles (files);      // spec decision A: files alone rebuild the device
    fromHeaders.name      = fromManifest.name;       // rig-level display fields live in the manifest…
    fromHeaders.modeledBy = fromManifest.modeledBy;  // …headers still carry them per-file (modeled_by)
    ok (! files.empty() && files[0].meta.count ("modeled_by") == 1, "headers carry modeled_by per file");
    checkRig (fromHeaders, "headers-only fallback");

    // A lone .namz carries no tone block — and that is CORRECT, not a loss: every file was
    // captured with the knob at `reference`, and the shipped description is relative to it. Without the
    // manifest a player simply plays the reference tone, which is what those weights encode.
    ok (fromHeaders.firstKnown() != nullptr && fromHeaders.firstKnown()->tone.empty(),
        "headers-only device has no tone block (it plays the reference position)");

    // --- 2. SELECT: the expectation table pins the policy --------------------------------------
    const auto* dev = &fromManifest.firstKnown()->device;
    for (const auto& row : expected["selection"])
    {
        const std::string from = row["from"].get<std::string>();
        Settings s;
        for (const auto& fe : dev->files) if (fe.id == from) s = fe.settings;
        const auto* hit = resolve (*dev, s, row["turn"][0].get<std::string>(), row["turn"][1].get<std::string>());
        if (row["expect"].is_null())
            ok (hit == nullptr, "selection: declared-but-uncaptured value selects nothing");
        else
            ok (hit != nullptr && hit->id == row["expect"].get<std::string>(), "selection row resolves");
    }

    // --- 3. WRITE: the model reproduces the golden bytes ---------------------------------------
    ok (writeManifest (model) == goldenManifest, "writeManifest reproduces rig.json BYTE-EXACT");
    for (const auto& f : files)
        ok (stampMeta (model, st, [&] { for (const auto& fe : st.device.files)
                                            if (fe.id == f.id) return fe.settings;
                                        return Settings {}; }()) == f.meta,
            "stampMeta reproduces the file's header keys exactly");
}

int main()
{
    std::printf ("namz rig conformance\n");
    const std::string root = NAMZ_CONFORMANCE_DIR;
    const bool regen = std::getenv ("NAMZ_REGEN") != nullptr;
    const auto flatNam = readFile (root + "/vectors/flat.nam");
    ok (! flatNam.empty(), "golden base model (vectors/flat.nam) present");

    runFixture (root + "/rig", flatNam, regen);
    runFixture (root + "/rig-measured", flatNam, regen);

    if (regen) { std::printf ("REGENERATED (re-run without NAMZ_REGEN to verify)\n"); return 0; }
    std::printf (failures == 0 ? "ALL RIG CONFORMANCE PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
