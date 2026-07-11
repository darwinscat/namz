// SPDX-License-Identifier: MIT
// namz_rig self-tests: controls-spec roundtrip, meta-driven + legacy-filename device building,
// defaults (noon gain / boost off), and the selection policy (pin the turned control, keep the
// rest, fall back toward defaults; settings snap to the chosen file's real combination).

#include "../include/namz_rig.h"

#include <cstdio>

using namespace namz::rig;

static int failures = 0;
static void ok (bool cond, const char* what)
{
    if (! cond) { ++failures; std::printf ("    FAIL: %s\n", what); }
}

int main()
{
    std::printf ("namz_rig tests\n");

    // --- controls spec roundtrip ---------------------------------------------------------------
    {
        const std::string spec = "channel:channel=green|orange|red; boost:boost=off|on; gain:gain=07h|08h|09h";
        const auto controls = parseControlsSpec (spec);
        ok (controls.size() == 3, "3 controls parsed");
        ok (controls[0].role == Role::Channel && controls[2].role == Role::Gain, "roles parsed");
        ok (controls[2].values.size() == 3 && controls[2].values[0] == "07h", "values in order");
        ok (buildControlsSpec (controls) == spec, "build(parse(spec)) == spec");
        ok (parseControlsSpec ("  ultralo : generic = -|off|+ ;; ").size() == 1, "tolerant of spacing/empties");
        ok (parseControlsSpec ("garbage").empty(), "junk rejected quietly");
    }

    // --- meta-driven device --------------------------------------------------------------------
    {
        const std::string spec = "channel:channel=green|red; boost:boost=off|on; gain:gain=07h|12h|17h";
        auto mk = [&spec] (std::string id, std::string ch, std::string b, std::string g) {
            FileMeta f;
            f.id = id; f.filenameBase = id;
            f.meta = { { "controls", spec }, { "gear_model", "ReVolt Guitar" }, { "gear_type", "pedal" },
                       { "rig_id", "dc-revolt" },
                       { "settings.channel", ch }, { "settings.boost", b }, { "settings.gain", g } };
            return f;
        };
        const auto devs = buildDevices ({ mk ("a", "green", "off", "07h"), mk ("b", "green", "off", "12h"),
                                          mk ("c", "red", "on", "17h") });
        ok (devs.size() == 1, "one device grouped by rig_id");
        const auto& d = devs.front();
        ok (d.family == "ReVolt Guitar" && d.rigId == "dc-revolt" && d.slot == "pedal",
            "family/rig_id/slot from meta");
        ok (d.controls.size() == 3 && d.files.size() == 3, "controls + files");

        auto s = defaultSettings (d);
        ok (s["gain"] == "12h" && s["boost"] == "off" && s["channel"] == "green",
            "defaults: noon gain, boost off, first channel");
        ok (d.find (s) != nullptr && d.find (s)->id == "b", "default combo resolves to file b");

        // Turn gain to 17h from defaults: exact (green,off,17h) was never captured; the policy pins
        // gain and picks the only 17h file (red/on) — settings snap to its real combo.
        const auto* f = resolve (d, s, "gain", "17h");
        ok (f != nullptr && f->id == "c", "gain=17h resolves to the captured 17h take");
        ok (s.at ("channel") == "red" && s.at ("boost") == "on", "settings snapped to the file's combo");

        // Back to 07h keeps us on green (exact match exists).
        const auto* f2 = resolve (d, s, "gain", "07h");
        ok (f2 != nullptr && f2->id == "a", "gain=07h exact match wins");
    }

    // --- legacy filename fallback ----------------------------------------------------------------
    {
        auto legacy = [] (std::string base) { return FileMeta { base, base, {} }; };
        const auto devs = buildDevices ({ legacy ("GtrVolt-blue-07h-boost"), legacy ("GtrVolt-green-12h"),
                                          legacy ("GtrVolt-blue-12h"), legacy ("Mark V") });
        ok (devs.size() == 2, "two families (GtrVolt + Mark V)");
        const auto* gv = &devs[devs[0].family == "GtrVolt" ? 0 : 1];
        ok (gv->family == "GtrVolt", "family from leftover tokens");
        bool haveCh = false, haveGain = false, haveBoost = false;
        for (const auto& c : gv->controls)
        {
            if (c.role == Role::Channel) { haveCh = true; ok (c.values.size() == 2, "2 channels"); }
            if (c.role == Role::Gain)    { haveGain = true; ok (c.values.front() == "07h", "gains clock-sorted"); }
            if (c.role == Role::Boost)   haveBoost = true;
        }
        ok (haveCh && haveGain && haveBoost, "channel/gain/boost dimensions detected");

        auto s = defaultSettings (*gv);
        ok (s["gain"] == "12h" && s["boost"] == "off", "legacy defaults");
        const auto* f = resolve (*gv, s, "channel", "blue");
        ok (f != nullptr && f->id == "GtrVolt-blue-12h", "channel turn keeps noon gain");
        const auto* fb = resolve (*gv, s, "boost", "on");
        ok (fb != nullptr && fb->id == "GtrVolt-blue-07h-boost", "boost=on jumps to the only boosted take");

        const auto* mk = &devs[devs[0].family == "Mark V" ? 0 : 1];
        ok (mk->controls.empty() && mk->files.size() == 1, "single plain file = zero visible controls");
    }

    // --- legacy topology (poweramps) --------------------------------------------------------------
    {
        auto legacy = [] (std::string base) { return FileMeta { base, base, {} }; };
        const auto devs = buildDevices ({ legacy ("6L6 PP 12h"), legacy ("6L6 SE 12h") });
        ok (devs.size() == 1, "PP/SE group as one family");
        bool topo = false;
        for (const auto& c : devs[0].controls) topo = topo || c.role == Role::Topology;
        ok (topo, "topology control detected from PP/SE tokens");
    }

    std::printf (failures == 0 ? "ALL RIG TESTS PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
