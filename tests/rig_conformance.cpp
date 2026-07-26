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
//   conformance/rig-measured/ one gain dial + a MEASURED (linear) tone knob — the minimal shape a
//                             player must handle: pick a model by the dial, apply the tone as DSP
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

    if (auto ms = sx.find ("measured"); ms != sx.end())
        for (const auto& mx : *ms)
        {
            Measured me;
            me.name      = mx["name"].get<std::string>();
            me.sweep     = mx["sweep"].get<int>();
            me.placement = mx["placement"].get<std::string>();
            me.reference = mx["reference"].get<std::string>();
            for (const auto& [k, v] : mx["operating_point"].items()) me.operatingPoint[k] = v.get<std::string>();
            me.grid.fLo    = mx["grid"]["f_lo"].get<double>();
            me.grid.fHi    = mx["grid"]["f_hi"].get<double>();
            if (auto t = mx.find ("trusted"); t != mx.end())
            {
                me.trusted.loHz   = (*t)["lo_hz"].get<double>();
                me.trusted.hiHz   = (*t)["hi_hz"].get<double>();
                me.trusted.spanDb = (*t)["span_db"].get<double>();
                me.trusted.levels = (*t)["levels"].get<int>();
            }
            me.grid.points = mx["grid"]["points"].get<int>();
            for (const auto& px : mx["positions"])
            {
                MeasuredPosition p;
                p.value      = px["value"].get<std::string>();
                p.norm       = px["norm"].get<double>();
                p.db         = px["db"].get<std::vector<double>>();
                me.positions.push_back (std::move (p));
            }
            st.measured.push_back (std::move (me));
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

// The measured block a player reads must equal the one the fixture declares — including the curve,
// which is the only thing that lets a better filter design ship later without re-measuring.
static void checkMeasured (const Stage& st, const json& sx, const char* via)
{
    char what[160];
    auto tag = [&what, via] (const char* w) { std::snprintf (what, sizeof (what), "%s (%s)", w, via); return what; };
    const auto ms = sx.find ("measured");
    const std::size_t want = ms == sx.end() ? 0 : ms->size();
    ok (st.measured.size() == want, tag ("measured count"));
    if (st.measured.size() != want) return;
    for (std::size_t i = 0; i < want; ++i)
    {
        const auto& me = st.measured[i];
        const auto& mx = (*ms)[i];
        ok (me.name == mx["name"].get<std::string>() && me.sweep == mx["sweep"].get<int>()
            && me.placement == mx["placement"].get<std::string>()
            && me.reference == mx["reference"].get<std::string>(), tag ("measured identity"));
        ok (me.grid.points == mx["grid"]["points"].get<int>(), tag ("measured grid"));
        if (auto t = mx.find ("trusted"); t != mx.end())
            ok (me.trusted.loHz == (*t)["lo_hz"].get<double>()
                && me.trusted.hiHz == (*t)["hi_hz"].get<double>()
                && me.trusted.spanDb == (*t)["span_db"].get<double>()
                && me.trusted.levels == (*t)["levels"].get<int>(),
                tag ("the band the curve may be applied in"));
        ok (me.positions.size() == mx["positions"].size(), tag ("measured position count"));
        if (me.positions.size() != mx["positions"].size()) continue;
        for (std::size_t p = 0; p < me.positions.size(); ++p)
        {
            const auto& got = me.positions[p];
            const auto& px  = mx["positions"][p];
            ok (got.value == px["value"].get<std::string>() && got.norm == px["norm"].get<double>(),
                tag ("measured position identity"));
            ok (got.db == px["db"].get<std::vector<double>>(), tag ("measured curve"));
        }
        // The knob is DSP, not an axis: it must never appear among the selectable controls.
        for (const auto& c : st.device.controls)
            ok (c.name != me.name, tag ("measured knob is not a selection control"));
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
    checkMeasured (*fromManifest.firstKnown(), expected["stage"], "manifest");

    Rig fromHeaders = loadRigFromFiles (files);      // spec decision A: files alone rebuild the device
    fromHeaders.name      = fromManifest.name;       // rig-level display fields live in the manifest…
    fromHeaders.modeledBy = fromManifest.modeledBy;  // …headers still carry them per-file (modeled_by)
    ok (! files.empty() && files[0].meta.count ("modeled_by") == 1, "headers carry modeled_by per file");
    checkRig (fromHeaders, "headers-only fallback");

    // A lone .namz carries no measured block — and that is CORRECT, not a loss: every file was
    // captured with the knob at `reference`, and the shipped curves are relative to it. Without the
    // manifest a player simply plays the reference tone, which is what those weights encode.
    ok (fromHeaders.firstKnown() != nullptr && fromHeaders.firstKnown()->measured.empty(),
        "headers-only device has no measured block (it plays the reference position)");

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
