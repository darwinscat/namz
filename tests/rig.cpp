// SPDX-License-Identifier: MIT
// namz_rig self-tests: controls-spec roundtrip, meta-driven + legacy-filename device building,
// defaults (noon gain / boost off), and the selection policy (pin the turned control, keep the
// rest, fall back toward defaults; settings snap to the chosen file's real combination).

#include "../include/namz_rig.h"
#include "../include/namz_rig_load.h"
#include "../include/namz_rig_write.h"

#include <cstdio>
#include <string>

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

    // --- resolve() contract hardening (crew) -----------------------------------------------------
    {
        Device d;
        d.controls = { { "gain", Role::Gain, {"07h","12h","17h"} } };
        d.files = { {"a", {{"gain","07h"}}}, {"b", {{"gain","12h"}}} };   // 17h NOT captured

        // CONTRACT: turning gain to an UNCAPTURED value must NOT return a file that contradicts the
        // pin — it returns nullptr and leaves settings untouched (turned control is law).
        Settings s = { {"gain","07h"} };
        const auto* r = resolve (d, s, "gain", "17h");
        ok (r == nullptr, "uncaptured turn -> nullptr (no file violates the pin)");
        ok (s.at ("gain") == "07h", "settings unchanged after a refused turn");

        // CONTRACT: on a score TIE, prefer the file sitting on the default. channel default = first
        // value "a"; both candidates match the turned control, tie on the rest -> pick the default.
        Device d2;
        d2.controls = { { "ch", Role::Channel, {"green","red"} }, { "boost", Role::Boost, {"off","on"} } };
        d2.files = { {"x", {{"ch","red"},{"boost","on"}}}, {"y", {{"ch","green"},{"boost","on"}}} };
        Settings s2 = { {"ch","green"}, {"boost","off"} };
        const auto* r2 = resolve (d2, s2, "boost", "on");   // both x,y have boost=on; tie on ch
        ok (r2 != nullptr && r2->id == "y", "tie broken toward the default channel (green), not first-seen");
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

    // --- rig.json manifest loading (theory-first falsification of the .orbitrig contracts) -----
    const std::string manifest = R"({
      "format": "orbitrig", "schema": 1,
      "rig_id": "dc-revolt", "name": "ReVolt Stack", "modeled_by": "Darwin's Cat",
      "chain": [
        { "kind": "nam", "slot": "preamp",
          "gear": {"make":"Two Notes","model":"ReVolt Guitar","type":"pedal"},
          "controls": [
            {"name":"channel","role":"channel","values":["green","red"]},
            {"name":"gain","role":"gain","values":["07h","12h","17h"]} ],
          "files": [
            {"file":"a.namz","settings":{"channel":"green","gain":"07h"}},
            {"file":"b.namz","settings":{"channel":"green","gain":"12h"}},
            {"file":"c.namz","settings":{"channel":"red","gain":"17h"}} ] },
        { "kind": "eq", "model": "fmv", "tone_only": true, "show_curve": false,
          "hidden": ["hpf","lpf"], "defaults": {"tight_hz":"120","bass":"0.5"} }
      ] })";

    // CONTRACT: manifest parses into the declared chain, in order, with gear/controls/files.
    {
        const auto rig = loadRigManifest (manifest);
        ok (rig.rigId == "dc-revolt" && rig.name == "ReVolt Stack", "manifest identity parsed");
        ok (rig.chain.size() == 2, "two stages in order");
        ok (rig.chain[0].kind == StageKind::Nam && rig.chain[0].slot == "preamp", "stage 0 = nam/preamp");
        ok (rig.chain[0].device.controls.size() == 2 && rig.chain[0].device.files.size() == 3,
            "controls + files from manifest");
        ok (rig.chain[0].model == "ReVolt Guitar", "gear model parsed");

        // CONTRACT: EQ is guidance only — hints round-trip.
        const auto& eq = rig.chain[1].eq;
        ok (rig.chain[1].kind == StageKind::Eq, "stage 1 = eq");
        ok (eq.model == "fmv" && eq.toneOnly && ! eq.showCurve, "eq flags parsed");
        ok (eq.hidden.size() == 2 && eq.defaults.at ("tight_hz") == "120", "eq hidden + defaults parsed");

        // CONTRACT: the selector works on a manifest-loaded Nam stage.
        auto s = defaultSettings (rig.chain[0].device);
        ok (s["gain"] == "12h", "noon-gain default on the manifest device");
        const auto* f = resolve (rig.chain[0].device, s, "gain", "17h");
        ok (f != nullptr && f->id == "c.namz", "turning gain resolves the captured file");
    }

    // CONTRACT: manifest is the SOURCE OF TRUTH (decision A) — its controls win over per-file meta.
    {
        // file meta claims a THIRD channel the manifest never lists; manifest must dominate.
        std::vector<FileMeta> metas = {
            { "a.namz", "a", { {"controls","channel:channel=green|red|blue"}, {"rig_id","dc-revolt"},
                               {"settings.channel","green"}, {"settings.gain","07h"} } } };
        const auto rig = loadRig (manifest, metas);
        ok (rig.chain.size() == 2 && rig.chain[0].device.controls[0].values.size() == 2,
            "manifest controls (2 channels) win over file meta (3 channels)");
    }

    // CONTRACT: an unknown stage kind is KEPT but SKIPPED, never fails the load.
    {
        const std::string withReverb = R"({"format":"orbitrig","chain":[
            {"kind":"reverb","mix":0.3},
            {"kind":"nam","slot":"preamp","controls":[{"name":"gain","role":"gain","values":["12h"]}],
             "files":[{"file":"x.namz","settings":{"gain":"12h"}}]} ]})";
        const auto rig = loadRigManifest (withReverb);
        ok (rig.chain.size() == 2, "unknown stage still occupies the chain");
        ok (rig.chain[0].kind == StageKind::Unknown && rig.chain[0].rawKind == "reverb",
            "unknown kind preserved verbatim");
        ok (rig.firstKnown() != nullptr && rig.firstKnown()->kind == StageKind::Nam,
            "firstKnown() skips the unknown stage");
    }

    // CONTRACT: a broken/absent manifest never crashes and falls back to file metas.
    {
        ok (loadRigManifest ("not json at all").chain.empty(), "garbage manifest -> empty chain");
        ok (loadRigManifest (R"({"format":"something-else","chain":[]})").chain.empty(),
            "wrong format -> empty chain");
        ok (loadRigManifest (R"({"format":"orbitrig","chain":"oops"})").chain.empty(),
            "chain not an array -> empty chain");

        // fallback path: no manifest, standalone .namz meta rebuilds a Nam stage.
        std::vector<FileMeta> metas = {
            { "g.namz", "g", { {"controls","gain:gain=07h|12h"}, {"gear_model","Solo"},
                               {"settings.gain","07h"} } },
            { "h.namz", "h", { {"controls","gain:gain=07h|12h"}, {"gear_model","Solo"},
                               {"settings.gain","12h"} } } };
        const auto rig = loadRig ("", metas);
        ok (rig.chain.size() == 1 && rig.chain[0].kind == StageKind::Nam, "fallback wraps a Nam stage");
        ok (rig.chain[0].device.files.size() == 2, "fallback carries both files");
        ok (rig.name == "Solo", "fallback names the rig after the device");
    }

    // CONTRACT (crew): wrong-typed optional EQ flags never crash the load — they fall to defaults.
    {
        const std::string bad = R"({"format":"orbitrig","chain":[
            {"kind":"eq","tone_only":"true","show_curve":0} ]})";
        const auto rig = loadRigManifest (bad);   // must not throw/crash
        ok (rig.chain.size() == 1 && ! rig.chain[0].eq.toneOnly && rig.chain[0].eq.showCurve,
            "non-boolean eq flags fall back to defaults, no crash");
    }

    // CONTRACT (crew): a structured (object/array) EQ default is skipped, never dump()ed —
    // a hostile deep-nested value must not overflow the stack.
    {
        std::string nested = std::string (2000, '[') + "0" + std::string (2000, ']');
        const std::string bomb = std::string (R"({"format":"orbitrig","chain":[{"kind":"eq","defaults":{"x":)")
                               + nested + R"(,"bass":"0.5"}}]})";
        const auto rig = loadRigManifest (bomb);   // must not crash
        ok (rig.chain.size() == 1, "deep-nested eq default did not crash the load");
        ok (rig.chain[0].eq.defaults.count ("x") == 0 && rig.chain[0].eq.defaults.at ("bass") == "0.5",
            "structured default skipped, scalar default kept");
    }

    // CONTRACT (crew): a VALID manifest with an empty chain is the source of truth — NOT a fallback
    // trigger (decision A). loadRig must return the empty rig, not rebuild from file metas.
    {
        std::vector<FileMeta> metas = {
            { "z.namz", "z", { {"controls","gain:gain=07h"}, {"gear_model","Should Not Appear"},
                               {"settings.gain","07h"} } } };
        const auto rig = loadRig (R"({"format":"orbitrig","name":"Empty","chain":[]})", metas);
        ok (rig.chain.empty() && rig.name == "Empty",
            "valid empty manifest wins over file metas (no spurious fallback)");
    }

    // CONTRACT (crew/Opus HIGH): int-typed settings/values in a manifest are NOT dropped — the
    // format types all-digit values to int, so a stage with numeric settings must stay selectable.
    {
        const std::string numeric = R"({"format":"orbitrig","chain":[{"kind":"nam","slot":"preamp",
            "controls":[{"name":"gain","role":"gain","values":[3,5,8]}],
            "files":[{"file":"a.namz","settings":{"gain":3}},
                     {"file":"b.namz","settings":{"gain":5}},
                     {"file":"c.namz","settings":{"gain":8}}]}]})";
        const auto rig = loadRigManifest (numeric);
        ok (rig.chain.size() == 1 && rig.chain[0].device.controls.size() == 1
                && rig.chain[0].device.controls[0].values.size() == 3,
            "numeric control values stringified, not dropped");
        auto& dev = rig.chain[0].device;
        auto s = defaultSettings (dev);
        const auto* f = resolve (dev, s, "gain", "8");
        ok (f != nullptr && f->id == "c.namz", "numeric settings resolve (stage is selectable)");
    }

    // CONTRACT (Opus MED): a legacy pack whose names are ALL tokens still forms ONE device.
    {
        auto legacy = [] (std::string base) { return FileMeta { base, base, {} }; };
        const auto devs = buildDevices ({ legacy ("blue-07h"), legacy ("red-07h"),
                                          legacy ("blue-12h"), legacy ("red-12h") });
        ok (devs.size() == 1, "token-only legacy files coalesce into one device");
        ok (devs[0].files.size() == 4, "all four combos in the one device");
    }

    // CONTRACT (Opus MED): rig_id stamped on only SOME files of a family still groups as one.
    {
        std::vector<FileMeta> metas = {
            { "a", "a", { {"controls","gain:gain=07h|12h"}, {"gear_model","ReVolt"}, {"rig_id","R"},
                          {"settings.gain","07h"} } },
            { "b", "b", { {"controls","gain:gain=07h|12h"}, {"gear_model","ReVolt"},
                          {"settings.gain","12h"} } } };   // same gear_model, NO rig_id
        const auto devs = buildDevices (metas);
        ok (devs.size() == 1 && devs[0].files.size() == 2,
            "partial rig_id stamping merges by gear_model (grouping survives)");
    }

    // CONTRACT: an IR stage carries its impulse file names.
    {
        const std::string ir = R"({"format":"orbitrig","chain":[
            {"kind":"ir","slot":"rig","files":["V30-57.wav","V30-121.wav"]} ]})";
        const auto rig = loadRigManifest (ir);
        ok (rig.chain.size() == 1 && rig.chain[0].kind == StageKind::Ir, "ir stage parsed");
        ok (rig.chain[0].irFiles.size() == 2, "ir files carried");
    }

    // --- WRITER (namz_rig_write.h): load(write(rig)) == rig for every carried field -------------
    {
        Rig rig;
        rig.rigId = "dc-revolt-guitar"; rig.name = "ReVolt Guitar"; rig.modeledBy = "Darwin's Cat";

        Stage nam;
        nam.kind = StageKind::Nam; nam.rawKind = "nam"; nam.slot = "preamp";
        nam.make = "Two Notes"; nam.model = "ReVolt Guitar"; nam.gearType = "pedal";
        nam.device.rigId = rig.rigId; nam.device.slot = nam.slot; nam.device.family = nam.model;
        nam.device.controls = parseControlsSpec ("channel:channel=green|red; boost:boost=off|on; gain:gain=07h|12h");
        nam.device.files = { { "ReVolt-green-07h.namz", { { "channel", "green" }, { "boost", "off" }, { "gain", "07h" } } },
                             { "ReVolt-red-12h.namz",   { { "channel", "red" },   { "boost", "off" }, { "gain", "12h" } } } };

        Stage eq;
        eq.kind = StageKind::Eq; eq.rawKind = "eq";
        eq.eq.model = "fmv"; eq.eq.toneOnly = true; eq.eq.showCurve = false;
        eq.eq.defaults = { { "tight_hz", "120" } }; eq.eq.hidden = { "hpf", "lpf" };

        Stage ir;
        ir.kind = StageKind::Ir; ir.rawKind = "ir"; ir.slot = "rig";
        ir.irFiles = { "V30-57.wav" };

        Stage odd;                         // a kind this model doesn't know — rawKind must survive
        odd.kind = StageKind::Unknown; odd.rawKind = "hologram"; odd.slot = "rig";

        rig.chain = { nam, eq, ir, odd };

        bool valid = false;
        const auto back = loadRigManifest (writeManifest (rig), &valid);
        ok (valid, "written manifest is a valid orbitrig manifest");
        ok (back.rigId == rig.rigId && back.name == rig.name && back.modeledBy == rig.modeledBy,
            "rig identity round-trips");
        ok (back.chain.size() == 4, "all four stages round-trip");
        if (back.chain.size() == 4)
        {
            const auto& n = back.chain[0];
            ok (n.kind == StageKind::Nam && n.slot == "preamp" && n.make == "Two Notes"
                && n.model == "ReVolt Guitar" && n.gearType == "pedal", "nam stage identity round-trips");
            ok (buildControlsSpec (n.device.controls) == buildControlsSpec (nam.device.controls),
                "controls round-trip in order");
            ok (n.device.files.size() == 2 && n.device.files[1].id == "ReVolt-red-12h.namz"
                && n.device.files[1].settings == nam.device.files[1].settings, "file index round-trips");
            const auto& e = back.chain[1];
            ok (e.kind == StageKind::Eq && e.eq.model == "fmv" && e.eq.toneOnly && ! e.eq.showCurve
                && e.eq.defaults.at ("tight_hz") == "120" && e.eq.hidden.size() == 2, "eq hints round-trip");
            ok (back.chain[2].kind == StageKind::Ir && back.chain[2].irFiles == ir.irFiles, "ir stage round-trips");
            ok (back.chain[3].kind == StageKind::Unknown && back.chain[3].rawKind == "hologram",
                "unknown stage keeps its rawKind (never silently dropped)");
        }

        // stampMeta → buildDevices closes the loop: per-file header keys a capture tool writes are
        // exactly what device building reads back — byte compatibility by construction.
        std::vector<FileMeta> metas;
        for (const auto& fe : nam.device.files)
        {
            FileMeta f;
            f.id = fe.id; f.filenameBase = fe.id;
            f.meta = stampMeta (rig, nam, fe.settings);
            metas.push_back (std::move (f));
        }
        ok (metas[0].meta.at ("controls") == buildControlsSpec (nam.device.controls), "stamped controls spec");
        ok (metas[0].meta.at ("rig_id") == "dc-revolt-guitar" && metas[0].meta.at ("slot") == "preamp"
            && metas[0].meta.at ("gear_make") == "Two Notes" && metas[0].meta.at ("modeled_by") == "Darwin's Cat",
            "stamped identity keys");
        ok (metas[0].meta.at ("settings.gain") == "07h" && metas[0].meta.at ("boost") == "false",
            "stamped positions + the conventional boost flag");
        const auto devs = buildDevices (metas);
        ok (devs.size() == 1 && devs[0].rigId == "dc-revolt-guitar"
            && devs[0].files.size() == 2 && devs[0].controls.size() == 3,
            "buildDevices reconstructs the device from stamped meta");
        Settings s = devs[0].files[0].settings;
        const auto* hit = resolve (devs[0], s, "gain", "12h");
        ok (hit != nullptr && hit->id == "ReVolt-red-12h.namz", "selection works over stamped meta");
    }

    // WRITER: a boosted file stamps boost=true; a rig with no boost control stamps no boost key.
    {
        Rig rig;
        Stage st;
        st.kind = StageKind::Nam;
        st.device.controls = parseControlsSpec ("boost:boost=off|on");
        ok (stampMeta (rig, st, { { "boost", "on" } }).at ("boost") == "true", "truthy boost stamps true");
        st.device.controls = parseControlsSpec ("gain:gain=07h|12h");
        ok (stampMeta (rig, st, { { "gain", "07h" } }).count ("boost") == 0, "no boost control → no boost key");
    }

    std::printf (failures == 0 ? "ALL RIG TESTS PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
