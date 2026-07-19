// SPDX-License-Identifier: MIT
// The reference consumer of the RIG conformance fixtures (conformance/rig/) — the executable form
// of the .orbitrig contract between a capture tool (writer) and a player (reader). Three duties:
// READ the golden pack into expected.json's device, SELECT per the expectation table (the fallback
// policy, not just the happy path), and WRITE the model back byte-exact (writeManifest == rig.json,
// stampMeta == each file's header). See conformance/rig/README.md for what consumers must run.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.

#define NAMZ_IMPLEMENTATION
#include "namz.h"
#include "namz_rig.h"
#include "namz_rig_load.h"
#include "namz_rig_write.h"

#include <nlohmann/json.hpp>

#include <cstdio>
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

int main()
{
    std::printf ("namz rig conformance\n");
    const std::string dir = std::string (NAMZ_CONFORMANCE_DIR) + "/rig";
    const json expected = json::parse (readFile (dir + "/expected.json"));
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

    Rig fromHeaders = loadRigFromFiles (files);      // spec decision A: files alone rebuild the device
    fromHeaders.name      = fromManifest.name;       // rig-level display fields live in the manifest…
    fromHeaders.modeledBy = fromManifest.modeledBy;  // …headers still carry them per-file (modeled_by)
    ok (! files.empty() && files[0].meta.count ("modeled_by") == 1, "headers carry modeled_by per file");
    checkRig (fromHeaders, "headers-only fallback");

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
    Rig model;
    model.rigId     = expected["rig"]["rig_id"].get<std::string>();
    model.name      = expected["rig"]["name"].get<std::string>();
    model.modeledBy = expected["rig"]["modeled_by"].get<std::string>();
    Stage st;
    st.kind = StageKind::Nam; st.rawKind = "nam";
    st.slot     = expected["stage"]["slot"].get<std::string>();
    st.make     = expected["stage"]["gear"]["make"].get<std::string>();
    st.model    = expected["stage"]["gear"]["model"].get<std::string>();
    st.gearType = expected["stage"]["gear"]["type"].get<std::string>();
    st.device.rigId = model.rigId; st.device.slot = st.slot; st.device.family = st.model;
    st.device.controls = parseControlsSpec (expected["stage"]["controls"].get<std::string>());
    for (const auto& [name, settings] : expected["stage"]["files"].items())
    {
        FileEntry fe;
        fe.id = name;
        for (const auto& [k, v] : settings.items()) fe.settings[k] = v.get<std::string>();
        st.device.files.push_back (std::move (fe));
    }
    model.chain = { st };

    ok (writeManifest (model) == goldenManifest, "writeManifest reproduces rig.json BYTE-EXACT");
    for (const auto& f : files)
        ok (stampMeta (model, st, [&] { for (const auto& fe : st.device.files)
                                            if (fe.id == f.id) return fe.settings;
                                        return Settings {}; }()) == f.meta,
            "stampMeta reproduces the file's header keys exactly");

    std::printf (failures == 0 ? "ALL RIG CONFORMANCE PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
