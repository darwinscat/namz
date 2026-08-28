// SPDX-License-Identifier: MIT
// namz_rig self-tests: controls-spec roundtrip, meta-driven + filename-token device building,
// defaults (mid-sweep gain / boost off), the selection policy (pin the turned control, keep the
// rest, fall back toward defaults; settings snap to the chosen file's real combination), and the
// tone (linear-knob) block in both its forms, and the schema gate. Knob positions are DEGREES
// throughout — the clock grammar is gone.

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
        const std::string spec = "channel:channel=green|orange|red; boost:boost=off|on; gain:gain=0|30|60";
        const auto controls = parseControlsSpec (spec);
        ok (controls.size() == 3, "3 controls parsed");
        ok (controls[0].role == Role::Channel && controls[2].role == Role::Gain, "roles parsed");
        ok (controls[2].values.size() == 3 && controls[2].values[0] == "0", "values in order");
        ok (buildControlsSpec (controls) == spec, "build(parse(spec)) == spec");
        ok (parseControlsSpec ("  ultralo : generic = -|off|+ ;; ").size() == 1, "tolerant of spacing/empties");
        ok (parseControlsSpec ("garbage").empty(), "junk rejected quietly");
    }

    // --- meta-driven device --------------------------------------------------------------------
    {
        const std::string spec = "channel:channel=green|red; boost:boost=off|on; gain:gain=0|150|300";
        auto mk = [&spec] (std::string id, std::string ch, std::string b, std::string g) {
            FileMeta f;
            f.id = id; f.filenameBase = id;
            f.meta = { { "controls", spec }, { "gear_model", "ReVolt Guitar" }, { "gear_type", "pedal" },
                       { "rig_id", "dc-revolt" },
                       { "settings.channel", ch }, { "settings.boost", b }, { "settings.gain", g } };
            return f;
        };
        const auto devs = buildDevices ({ mk ("a", "green", "off", "0"), mk ("b", "green", "off", "150"),
                                          mk ("c", "red", "on", "300") });
        ok (devs.size() == 1, "one device grouped by rig_id");
        const auto& d = devs.front();
        ok (d.family == "ReVolt Guitar" && d.rigId == "dc-revolt" && d.slot == "pedal",
            "family/rig_id/slot from meta");
        ok (d.controls.size() == 3 && d.files.size() == 3, "controls + files");

        auto s = defaultSettings (d);
        ok (s["gain"] == "150" && s["boost"] == "off" && s["channel"] == "green",
            "defaults: mid-sweep gain, boost off, first channel");
        ok (d.find (s) != nullptr && d.find (s)->id == "b", "default combo resolves to file b");

        // Turn gain to 300° from defaults: exact (green,off,300) was never captured; the policy pins
        // gain and picks the only 300° file (red/on) — settings snap to its real combo.
        const auto* f = resolve (d, s, "gain", "300");
        ok (f != nullptr && f->id == "c", "gain=300 resolves to the captured 300° take");
        ok (s.at ("channel") == "red" && s.at ("boost") == "on", "settings snapped to the file's combo");

        // Back to 0° keeps us on green (exact match exists).
        const auto* f2 = resolve (d, s, "gain", "0");
        ok (f2 != nullptr && f2->id == "a", "gain=0 exact match wins");
    }

    // --- a filename is a caption, never evidence -------------------------------------------------
    // FALSIFICATION of the whole former filename grammar: names that LOOK like the old token
    // language ("colour", "boost", "PP", "12h", a bare number) must produce no controls whatsoever.
    // A reader that still infers from names would grow a channel/boost/gain dial here and fail.
    {
        auto plain = [] (std::string base) { return FileMeta { base, base, {} }; };
        const auto devs = buildDevices ({ plain ("GtrVolt-blue-boost"), plain ("GtrVolt-green"),
                                          plain ("GtrVolt-blue"), plain ("Mark V") });
        ok (devs.size() == 4, "no metadata = no family: every file stands alone");
        for (const auto& d : devs)
            ok (d.controls.empty() && d.files.size() == 1, "a nameless file is a device with no knobs");

        const auto num = buildDevices ({ plain ("Peavey 120"), plain ("6L6 PP 12h") });
        ok (num.size() == 2 && num[0].controls.empty() && num[1].controls.empty(),
            "numbers, clock tokens and PP/SE in a name mean nothing");
        ok (num[0].family == "Peavey 120", "the name survives as the caption");

        // …while the SAME files, once stamped, do form one device. Metadata is the only evidence.
        std::vector<FileMeta> stamped = {
            { "a.namz", "a", { { "controls", "boost:boost=off|on" }, { "gear_model", "GtrVolt" },
                               { "settings.boost", "off" } } },
            { "b.namz", "b", { { "controls", "boost:boost=off|on" }, { "gear_model", "GtrVolt" },
                               { "settings.boost", "on" } } } };
        const auto real = buildDevices (stamped);
        ok (real.size() == 1 && real[0].controls.size() == 1 && real[0].files.size() == 2,
            "stamped files build the device the names could not");
    }

    // --- resolve() contract hardening (crew) -----------------------------------------------------
    {
        Device d;
        d.controls = { { "gain", Role::Gain, {"0","150","300"}, {}, 300 } };
        d.files = { {"a", {{"gain","0"}}}, {"b", {{"gain","150"}}} };   // 300 NOT captured

        // CONTRACT: turning gain to an UNCAPTURED value must NOT return a file that contradicts the
        // pin — it returns nullptr and leaves settings untouched (turned control is law).
        Settings s = { {"gain","0"} };
        const auto* r = resolve (d, s, "gain", "300");
        ok (r == nullptr, "uncaptured turn -> nullptr (no file violates the pin)");
        ok (s.at ("gain") == "0", "settings unchanged after a refused turn");

        // CONTRACT: on a score TIE, prefer the file sitting on the default. channel default = first
        // value "a"; both candidates match the turned control, tie on the rest -> pick the default.
        Device d2;
        d2.controls = { { "ch", Role::Channel, {"green","red"} }, { "boost", Role::Boost, {"off","on"} } };
        d2.files = { {"x", {{"ch","red"},{"boost","on"}}}, {"y", {{"ch","green"},{"boost","on"}}} };
        Settings s2 = { {"ch","green"}, {"boost","off"} };
        const auto* r2 = resolve (d2, s2, "boost", "on");   // both x,y have boost=on; tie on ch
        ok (r2 != nullptr && r2->id == "y", "tie broken toward the default channel (green), not first-seen");
    }

    // --- topology (poweramps) is a stamped control like any other --------------------------------
    {
        auto mk = [] (std::string id, std::string topo) {
            return FileMeta { id, id, { { "controls", "topology:topology=PP|SE" },
                                        { "gear_model", "6L6" }, { "settings.topology", topo } } };
        };
        const auto devs = buildDevices ({ mk ("a", "PP"), mk ("b", "SE") });
        ok (devs.size() == 1, "one 6L6 device");
        bool topo = false;
        for (const auto& c : devs[0].controls) topo = topo || c.role == Role::Topology;
        ok (topo, "topology control comes from the stamped spec");
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
            {"name":"gain","role":"gain","sweep":300,"values":["0","150","300"]} ],
          "files": [
            {"file":"a.namz","settings":{"channel":"green","gain":"0"}},
            {"file":"b.namz","settings":{"channel":"green","gain":"150"}},
            {"file":"c.namz","settings":{"channel":"red","gain":"300"}} ] },
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
        ok (rig.chain[0].device.controls[1].sweep == 300 && rig.chain[0].device.controls[0].sweep == 0,
            "sweep parsed for the dial, absent for the channel switch");
        ok (rig.chain[0].model == "ReVolt Guitar", "gear model parsed");

        // CONTRACT: EQ is guidance only — hints round-trip.
        const auto& eq = rig.chain[1].eq;
        ok (rig.chain[1].kind == StageKind::Eq, "stage 1 = eq");
        ok (eq.model == "fmv" && eq.toneOnly && ! eq.showCurve, "eq flags parsed");
        ok (eq.hidden.size() == 2 && eq.defaults.at ("tight_hz") == "120", "eq hidden + defaults parsed");

        // CONTRACT: the selector works on a manifest-loaded Nam stage.
        auto s = defaultSettings (rig.chain[0].device);
        ok (s["gain"] == "150", "mid-sweep gain default on the manifest device");
        const auto* f = resolve (rig.chain[0].device, s, "gain", "300");
        ok (f != nullptr && f->id == "c.namz", "turning gain resolves the captured file");
    }

    // CONTRACT: manifest is the SOURCE OF TRUTH (decision A) — its controls win over per-file meta.
    {
        // file meta claims a THIRD channel the manifest never lists; manifest must dominate.
        std::vector<FileMeta> metas = {
            { "a.namz", "a", { {"controls","channel:channel=green|red|blue"}, {"rig_id","dc-revolt"},
                               {"settings.channel","green"}, {"settings.gain","0"} } } };
        const auto rig = loadRig (manifest, metas);
        ok (rig.chain.size() == 2 && rig.chain[0].device.controls[0].values.size() == 2,
            "manifest controls (2 channels) win over file meta (3 channels)");
    }

    // CONTRACT: an unknown stage kind is KEPT but SKIPPED, never fails the load.
    {
        const std::string withReverb = R"({"format":"orbitrig","chain":[
            {"kind":"reverb","mix":0.3},
            {"kind":"nam","slot":"preamp","controls":[{"name":"gain","role":"gain","values":["150"]}],
             "files":[{"file":"x.namz","settings":{"gain":"150"}}]} ]})";
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
            { "g.namz", "g", { {"controls","gain:gain=0|150"}, {"gear_model","Solo"},
                               {"settings.gain","0"} } },
            { "h.namz", "h", { {"controls","gain:gain=0|150"}, {"gear_model","Solo"},
                               {"settings.gain","150"} } } };
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
            { "z.namz", "z", { {"controls","gain:gain=0"}, {"gear_model","Should Not Appear"},
                               {"settings.gain","0"} } } };
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

    // CONTRACT: files with no `controls` spec but a shared gear_model still form ONE entry — the
    // grouping key is metadata, so an unstamped family stays together instead of littering the list.
    {
        auto bare = [] (std::string id) {
            return FileMeta { id, id, { { "gear_model", "Nameless" } } };
        };
        const auto devs = buildDevices ({ bare ("a"), bare ("b") });
        ok (devs.size() == 1 && devs[0].files.size() == 2 && devs[0].controls.empty(),
            "gear_model groups unstamped files; no spec still means no knobs");
    }

    // CONTRACT (Opus MED): rig_id stamped on only SOME files of a family still groups as one.
    {
        std::vector<FileMeta> metas = {
            { "a", "a", { {"controls","gain:gain=0|150"}, {"gear_model","ReVolt"}, {"rig_id","R"},
                          {"settings.gain","0"} } },
            { "b", "b", { {"controls","gain:gain=0|150"}, {"gear_model","ReVolt"},
                          {"settings.gain","150"} } } };   // same gear_model, NO rig_id
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

    // --- TONE controls: a linear knob shipped as DSP, never as a selection axis -------------------
    {
        const std::string withTone = R"({"format":"orbitrig","chain":[
            {"kind":"nam","slot":"pedal",
             "controls":[{"name":"gain","role":"gain","sweep":300,"values":["0","150","300"]}],
             "tone":[{"name":"tone","sweep":300,"placement":"post","reference":"300",
                          "operating_point":{"gain":"150"},
                          "grid":{"f_lo":20,"f_hi":20000,"points":4},
                          "trusted":{"lo_hz":40,"hi_hz":9000,"span_db":24,"levels":8},
                          "positions":[{"value":"0","norm":0.0,"db":[0.0,-0.5,-6.0,-18.0]},
                                       {"value":"300","norm":1.0,"db":[0.0,0.0,0.0,0.0]}]}],
             "blend":[{"name":"blend","sweep":300,"reference":"300","dry_end":"0",
                       "law":"linear","polarity":-1,
                       "grid":{"f_lo":20,"f_hi":20000,"points":4},
                       "dry":[0.0,-1.0,-9.0,-24.0],"dry_level_db":-9.0,
                       "positions":[{"value":"0","norm":0.0,"dry_db":0.0,"wet_db":-120.0},
                                    {"value":"150","norm":0.5,"dry_db":-6.0,"wet_db":-6.0},
                                    {"value":"300","norm":1.0,"dry_db":-120.0,"wet_db":0.0}]}],
             "files":[{"file":"a.namz","settings":{"gain":"0"}}]}]})";
        const auto rig = loadRigManifest (withTone);
        ok (rig.chain.size() == 1 && rig.chain[0].tone.size() == 1, "tone block parsed");
        const auto& me = rig.chain[0].tone.front();
        ok (me.name == "tone" && me.sweep == 300 && me.placement == "post" && me.reference == "300",
            "tone identity: name / sweep / placement / reference");
        ok (me.operatingPoint.at ("gain") == "150", "the operating point held while sweeping is carried");
        ok (me.grid.points == 4 && me.grid.fLo == 20.0 && me.grid.fHi == 20000.0, "curve grid parsed");
        ok (me.trusted.loHz == 40.0 && me.trusted.hiHz == 9000.0 && me.trusted.spanDb == 24.0
            && me.trusted.levels == 8,
            "the band the curve was shown to be a filter in, and how many levels showed it");
        ok (me.positions.size() == 2 && me.positions[0].value == "0" && me.positions[0].norm == 0.0,
            "positions parse in the control's own order");
        ok (me.positions[0].db.size() == 4 && me.positions[0].db[3] == -18.0,
            "the measured ladder is carried — with `positions`, it is the whole description of the control");

        // A blend is the THIRD kind of control: not a filter (so not a `tone`) and not an axis (so not
        // in `controls`). What a reader needs is the dry path's response and the two gains per position.
        ok (rig.chain[0].blend.size() == 1, "the blend block parsed");
        if (rig.chain[0].blend.size() == 1)
        {
            const auto& bl = rig.chain[0].blend.front();
            ok (bl.name == "blend" && bl.sweep == 300 && bl.reference == "300" && bl.dryEnd == "0",
                "identity: the models were captured at the WET end, the curve measured at the DRY one");
            ok (bl.polarity == -1, "a box that inverts its dry path says so — the centre of the knob "
                                   "depends on it");
            ok (bl.law == "linear", "the law rides along as provenance, never as something to implement");
            ok (bl.dryDb.size() == 4 && bl.dryDb[3] == -24.0,
                "the dry path's own response is carried: on a bass pedal it is never flat");
            ok (bl.dryLevelDb == -9.0,
                "…and its broadband LEVEL, which no reader could ever re-measure");
            ok (bl.positions.size() == 3 && bl.positions[1].dryDb == -6.0 && bl.positions[1].wetDb == -6.0,
                "…and the two gains are shipped per position, so a peculiar pot taper is expressible");
            ok (bl.positions[0].wetDb == -120.0 && bl.positions[2].dryDb == -120.0,
                "the ends are silent on one side each");
        }

        // FALSIFICATION: the tone knob must NOT become a selection control. No file carries it in
        // `settings`, so a dial built from it would pin a value resolve() can never satisfy — the
        // player would show a knob that refuses every turn.
        ok (rig.chain[0].device.controls.size() == 1
            && rig.chain[0].device.controls[0].name == "gain",
            "tone is NOT a selection control (the axes stay what the files carry)");

        // Junk entries are dropped, not half-loaded: no name / no positions = nothing to apply.
        const auto junk = loadRigManifest (R"({"format":"orbitrig","chain":[{"kind":"nam",
            "tone":[{"sweep":300},{"name":"tone"},
                    {"name":"ok","grid":{"points":1},"positions":[{"value":"0","db":[0.0]}]}]}]})");
        ok (junk.chain.size() == 1 && junk.chain[0].tone.size() == 1
            && junk.chain[0].tone[0].name == "ok", "nameless / empty tone entries dropped");
        ok (junk.chain.size() == 1 && junk.chain[0].tone.size() == 1
            && junk.chain[0].tone[0].placement == "post", "placement defaults to post");

        // A position IS its curve: no `db`, a `db` of the wrong length, or one with a hole in it is
        // skipped — not padded, not truncated — and a knob with no grid, or with nothing left, is
        // dropped. Until now all of it loaded, and a reader found the short curve by indexing past it.
        {
            const auto r = loadRigManifest (R"({"format":"orbitrig","chain":[{"kind":"nam","tone":[
                {"name":"nogrid","sweep":300,"reference":"300",
                 "positions":[{"value":"0","db":[0,-6]},{"value":"300","db":[0,0]}]},
                {"name":"short","sweep":300,"reference":"300","grid":{"points":3},
                 "positions":[{"value":"0","db":[0,-6]},{"value":"150","db":[0,-3,-9]},{"value":"300"},
                              {"value":"200","db":[0,"x",-4]},{"value":"250","db":[0,-1,-2,-3]}]},
                {"name":"none","sweep":300,"reference":"300","grid":{"points":3},
                 "positions":[{"value":"0","db":[0]}]}
            ]}]})");
            ok (r.chain.size() == 1 && r.chain[0].tone.size() == 1 && r.chain[0].tone[0].name == "short",
                "a knob with no grid, and one with no usable position, are dropped");
            ok (r.chain.size() == 1 && r.chain[0].tone.size() == 1 && r.chain[0].tone[0].positions.size() == 1
                && r.chain[0].tone[0].positions[0].value == "150",
                "short, missing, holed and overlong curves are skipped; the exact one stays");
        }
    }

    // --- TONE, the second form: `sections` — the knob as bands, ONE form per knob -----------------
    {
        auto manifestWith = [] (const std::string& toneJson) {
            return std::string (R"({"format":"orbitrig","chain":[{"kind":"nam","slot":"pedal",
                "controls":[{"name":"gain","role":"gain","sweep":300,"values":["0","300"]}],
                "files":[{"file":"a.namz","settings":{"gain":"0"}}],
                "tone":[)") + toneJson + "]}]}";
        };
        const std::string bass = R"({"name":"bass","sweep":300,"placement":"post","reference":"150",
            "operating_point":{"gain":"150"},
            "sections":[{"type":"low_shelf","hz":167,"q":0.58,"range_db":[-12.0,6.0],"hz_at":[120,240],"q_at":[0.5,0.7]},
                        {"type":"tilt","hz":1077,"q":0.65,"pivot":0.35,"range_db":[-3.0,1.5]},
                        {"type":"bell","hz":800,"q":1.2,"range_db":[-2.0,2.0],"pivot":"junk"},
                        {"type":"high_shelf","hz":3000,"q":0.7,"range_db":[0.0,4.0],"hz_at":[3000,2200]}]})";
        bool valid = false;
        const auto rig = loadRigManifest (manifestWith (bass), &valid);
        ok (valid && rig.chain.size() == 1 && rig.chain[0].tone.size() == 1, "a sections knob parses");
        if (rig.chain.size() == 1 && rig.chain[0].tone.size() == 1)
        {
            const auto& t = rig.chain[0].tone[0];
            ok (t.name == "bass" && t.sweep == 300 && t.reference == "150" && t.positions.empty()
                && t.grid.points == 0, "identity, and no ladder beside the bands");
            ok (t.sections.size() == 4, "every band read");
            if (t.sections.size() == 4)
            {
                const auto& ls = t.sections[0];
                ok (ls.kind == SectionKind::LowShelf && ls.hz == 167.0 && ls.q == 0.58,
                    "low shelf: kind, and hz / q at the reference");
                ok (ls.dbAtMin == -12.0 && ls.dbAtMax == 6.0, "range_db: SIGNED, the minus stop first");
                ok (ls.hzAtMin == 120.0 && ls.hzAtMax == 240.0 && ls.qAtMin == 0.5 && ls.qAtMax == 0.7,
                    "the travel pair, both sides, in position order");
                ok (ls.pivot == 0.5, "a shelf's pivot is never touched");
                const auto& tl = t.sections[1];
                ok (tl.kind == SectionKind::Tilt && tl.pivot == 0.35, "a tilt's hinge is read as fitted, not assumed");
                ok (tl.hzAtMin == 0.0 && tl.hzAtMax == 0.0 && tl.qAtMin == 0.0 && tl.qAtMax == 0.0,
                    "no travel key = does not move, 0 in the model");
                ok (t.sections[2].kind == SectionKind::Bell && t.sections[2].pivot == 0.5,
                    "a stray `pivot` on a bell is not read — the band survives it");
                ok (t.sections[3].hzAtMin == 0.0 && t.sections[3].hzAtMax == 2200.0,
                    "a side spelled as the reference value reads back as 0: one spelling for `does not move`");
            }
        }

        // A tilt without `pivot` is the symmetric seesaw.
        {
            const auto r = loadRigManifest (manifestWith (R"({"name":"t","sweep":300,"reference":"0",
                "sections":[{"type":"tilt","hz":1000,"q":0.7,"range_db":[0.0,6.0]}]})"));
            ok (r.chain.size() == 1 && r.chain[0].tone.size() == 1 && r.chain[0].tone[0].sections.size() == 1
                && r.chain[0].tone[0].sections[0].pivot == 0.5, "a tilt without pivot hinges at 0.5");
        }

        // ONE FORM PER KNOB: both keys on one knob is an invalid manifest — refused whole, ok=false —
        // even when one of them is empty. Presence is the offence.
        {
            const std::string bothForms = R"({"name":"x","sweep":300,"reference":"300",
                "positions":[],"sections":[{"type":"bell","hz":800,"q":1,"range_db":[-1,1]}]})";
            bool okBoth = true;
            const auto r = loadRigManifest (manifestWith (bothForms), &okBoth);
            ok (! okBoth && r.chain.empty(), "both forms on one knob: the manifest is refused whole");
            // …and through loadRig that is the headers-only fallback, exactly as for a schema this
            // reader does not speak — the existing contract, not a third behaviour.
            FileMeta f; f.id = "a.namz"; f.filenameBase = "a";
            f.meta = { { "controls", "gain:gain=0|300" }, { "settings.gain", "0" }, { "rig_id", "x" } };
            const auto fb = loadRig (manifestWith (bothForms), { f });
            ok (fb.chain.size() == 1 && fb.chain[0].tone.empty() && fb.chain[0].device.files.size() == 1,
                "loadRig falls back to the headers: a device with no tone knobs");
        }

        // A knob this reader cannot build is DROPPED — the manifest stays valid, the knob plays flat.
        auto dropped = [&] (const char* toneJson, const char* why) {
            bool v = false;
            const auto r = loadRigManifest (manifestWith (toneJson), &v);
            ok (v && r.chain.size() == 1 && r.chain[0].tone.empty(), why);
        };
        dropped (R"({"name":"x","sweep":300,"reference":"0","sections":[{"type":"notch","hz":800,"q":1,"range_db":[-1,1]}]})",
                 "unknown band type: the whole knob is skipped, not just the band");
        dropped (R"({"name":"x","sweep":300,"reference":"0","sections":[{"type":"bell","hz":800,"q":1,"range_db":[-1,1]},{"type":"bell","hz":900,"q":1}]})",
                 "a band without range_db spoils the knob, the readable bands before it included");
        dropped (R"({"name":"x","sweep":300,"reference":"0","sections":[{"type":"bell","hz":0,"q":1,"range_db":[-1,1]}]})",
                 "a band with no frequency: unusable");
        dropped (R"({"name":"x","sweep":300,"reference":"0","sections":[{"type":"bell","hz":800,"q":1,"range_db":[-1,1],"hz_at":[0,900]}]})",
                 "a 0 in hz_at is not a frequency: unusable");
        dropped (R"({"name":"x","sweep":300,"reference":"0","sections":[{"type":"bell","hz":800,"q":1,"range_db":[-1,1],"q_at":[1]}]})",
                 "a travel pair that is not a pair: unusable");
        dropped (R"({"name":"x","sweep":300,"reference":"0","sections":[{"type":"tilt","hz":800,"q":1,"range_db":[-1,1],"pivot":"half"}]})",
                 "a tilt whose pivot is not a number: unusable");
        dropped (R"({"name":"x","reference":"0","sections":[{"type":"bell","hz":800,"q":1,"range_db":[-1,1]}]})",
                 "sections without a sweep: the travel law has nothing to run on");
        dropped (R"({"name":"x","sweep":300,"reference":"noon","sections":[{"type":"bell","hz":800,"q":1,"range_db":[-1,1]}]})",
                 "a reference that is not a position on the dial: unusable");
        dropped (R"({"name":"x","sweep":300,"reference":"301","sections":[{"type":"bell","hz":800,"q":1,"range_db":[-1,1]}]})",
                 "a reference past the sweep: unusable");
        dropped (R"({"name":"x","sweep":300,"reference":"0","sections":[]})",
                 "an empty sections array is a knob with neither form: dropped");
    }

    // --- THE SCHEMA GATE — written with the format, read by nobody, tested for the first time here --
    {
        auto withSchema = [] (const std::string& schema) {
            return std::string (R"({"format":"orbitrig","schema":)") + schema + R"(,"chain":[{"kind":"nam"}]})";
        };
        bool v = false;
        auto r = loadRigManifest (withSchema (std::to_string (kRigSchema)), &v);
        ok (v && r.chain.size() == 1, "this reader's own schema loads");
        v = true;
        r = loadRigManifest (withSchema (std::to_string (kRigSchema + 1)), &v);   // never a literal: must not rot
        ok (! v && r.chain.empty(), "a schema one above this reader's is refused, whole");
        v = false;
        r = loadRigManifest (withSchema ("1"), &v);
        ok (v && r.chain.size() == 1, "an older schema still loads: its keys are a subset");
        // Not an integer is not a number this reader may judge. `jint` used to answer 1 for these two,
        // which walked a pack of unknown vintage through the gate as the oldest one there is.
        v = true;
        r = loadRigManifest (withSchema (std::to_string (kRigSchema + 1) + ".0"), &v);
        ok (! v && r.chain.empty(), "a float schema is refused, not read as the oldest vintage");
        v = true;
        r = loadRigManifest (withSchema ("\"4\""), &v);
        ok (! v && r.chain.empty(), "a string schema is refused too");
    }

    // --- WRITER (namz_rig_write.h): load(write(rig)) == rig for every carried field -------------
    {
        Rig rig;
        rig.rigId = "dc-revolt-guitar"; rig.name = "ReVolt Guitar"; rig.modeledBy = "Darwin's Cat";

        Stage nam;
        nam.kind = StageKind::Nam; nam.rawKind = "nam"; nam.slot = "preamp";
        nam.make = "Two Notes"; nam.model = "ReVolt Guitar"; nam.gearType = "pedal";
        nam.device.rigId = rig.rigId; nam.device.slot = nam.slot; nam.device.family = nam.model;
        nam.device.controls = parseControlsSpec ("channel:channel=green|red; boost:boost=off|on; gain:gain=0|150");
        nam.device.controls.back().sweep = 300;                       // the gain dial's rotation
        nam.device.files = { { "ReVolt-green-0.namz", { { "channel", "green" }, { "boost", "off" }, { "gain", "0" } } },
                             { "ReVolt-red-150.namz",   { { "channel", "red" },   { "boost", "off" }, { "gain", "150" } } } };

        Tone tone;                                                // a linear knob: DSP, not an axis
        tone.name = "tone"; tone.sweep = 300; tone.reference = "300";
        tone.operatingPoint = { { "gain", "150" } };
        tone.grid.points = 3;
        tone.trusted = { 40.0, 9000.0, 0, 0, 24.0, 8 };
        tone.positions = { { "0",   {}, 0.0, 0.0, { 0.0, -6.0, -18.0 } },
                           { "300", {}, 1.0, 0.0, { 0.0,  0.0,   0.0 } } };

        Tone bass;                                                    // the same kind of knob, as bands
        bass.name = "bass"; bass.sweep = 300; bass.reference = "150";
        //                kind                   hz     q     dbMin  dbMax pivot hzMin hzMax  qMin qMax
        bass.sections = { { SectionKind::LowShelf, 167.0, 0.58, -12.0, 6.0, 0.5, 0.0, 240.0, 0.5, 0.0 },
                          { SectionKind::Tilt,     1077.0, 0.65, -3.0,  1.5, 0.35 } };
        nam.tone = { tone, bass };

        Blend mix;                                                    // the third kind of control
        mix.name = "blend"; mix.sweep = 300; mix.reference = "300"; mix.dryEnd = "0";
        mix.law = "equal_power"; mix.polarity = -1;
        mix.grid.points = 3;
        mix.dryDb = { 0.0, -2.0, -12.0 };
        mix.dryLevelDb = -4.5;
        mix.positions = { { "0", {}, 0.0, 0.0, -120.0 }, { "300", {}, 1.0, -120.0, 0.0 } };
        nam.blend = { mix };

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
            ok (n.device.files.size() == 2 && n.device.files[1].id == "ReVolt-red-150.namz"
                && n.device.files[1].settings == nam.device.files[1].settings, "file index round-trips");
            ok (n.device.controls.back().sweep == 300, "the dial's sweep round-trips");
            ok (n.tone.size() == 2 && n.tone[0].name == "tone" && n.tone[0].sweep == 300
                && n.tone[0].reference == "300" && n.tone[0].placement == "post"
                && n.tone[0].operatingPoint.at ("gain") == "150",
                "tone identity round-trips");
            ok (n.tone.size() == 2 && n.tone[0].positions.size() == 2
                && n.tone[0].positions[0].db == std::vector<double> { 0.0, -6.0, -18.0 }
                && n.tone[0].grid.points == 3
                && n.tone[0].trusted.hiHz == 9000.0 && n.tone[0].trusted.spanDb == 24.0
                && n.tone[0].trusted.levels == 8,
                "the ladder round-trips, grid and trusted band and all");
            ok (n.tone.size() == 2 && n.tone[1].name == "bass" && n.tone[1].positions.empty()
                && n.tone[1].grid.points == 0 && n.tone[1].sections.size() == 2,
                "the sections knob round-trips as sections only: no ladder, no grid");
            if (n.tone.size() == 2 && n.tone[1].sections.size() == 2)
            {
                const auto& ls = n.tone[1].sections[0];
                ok (ls.kind == SectionKind::LowShelf && ls.hz == 167.0 && ls.q == 0.58
                    && ls.dbAtMin == -12.0 && ls.dbAtMax == 6.0, "a band's kind and signed range round-trip");
                // One-sided travel: the model said 0 on one side, the pack spells that as the reference
                // value, and the loader spells it back as 0 — field-exact, not merely equivalent.
                ok (ls.hzAtMin == 0.0 && ls.hzAtMax == 240.0 && ls.qAtMin == 0.5 && ls.qAtMax == 0.0,
                    "one-sided travel round-trips field-exact through the reference-value spelling");
                ok (n.tone[1].sections[1].kind == SectionKind::Tilt && n.tone[1].sections[1].pivot == 0.35,
                    "a tilt's fitted hinge round-trips");
            }
            const auto text = writeManifest (rig);
            ok (text.find ("\"measured\"") == std::string::npos && text.find ("\"tone\"") != std::string::npos,
                "the key is `tone`; `measured` is gone");
            ok (text.find ("\"schema\": " + std::to_string (kRigSchema)) != std::string::npos,
                "the writer spells the schema from the constant");
            ok (n.blend.size() == 1 && n.blend[0].polarity == -1 && n.blend[0].law == "equal_power"
                && n.blend[0].dryEnd == "0" && n.blend[0].dryDb == std::vector<double> { 0.0, -2.0, -12.0 }
                && n.blend[0].dryLevelDb == -4.5
                && n.blend[0].positions.size() == 2 && n.blend[0].positions[1].wetDb == 0.0,
                "the blend block round-trips: polarity, law, dry curve and both gains");
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
        ok (metas[0].meta.at ("settings.gain") == "0" && metas[0].meta.at ("boost") == "false",
            "stamped positions + the conventional boost flag");
        ok (metas[0].meta.at ("sweep.gain") == "300" && metas[0].meta.count ("sweep.channel") == 0,
            "the dial's sweep is stamped per control; a switch gets none");
        const auto devs = buildDevices (metas);
        ok (devs.size() == 1 && devs[0].rigId == "dc-revolt-guitar"
            && devs[0].files.size() == 2 && devs[0].controls.size() == 3,
            "buildDevices reconstructs the device from stamped meta");
        Settings s = devs[0].files[0].settings;
        const auto* hit = resolve (devs[0], s, "gain", "150");
        ok (hit != nullptr && hit->id == "ReVolt-red-150.namz", "selection works over stamped meta");
    }

    // WRITER: one form per knob, and a knob the reader would drop on sight is not written at all.
    {
        Rig rig;
        Stage st;
        st.kind = StageKind::Nam; st.rawKind = "nam";
        Tone both;                                                    // a model holding BOTH forms
        both.name = "both"; both.sweep = 300; both.reference = "300";
        both.grid.points = 2;
        both.positions = { { "0", {}, 0.0, 0.0, { 0.0, -6.0 } }, { "300", {}, 1.0, 0.0, { 0.0, 0.0 } } };
        both.sections  = { { SectionKind::Bell, 800.0, 1.0, -2.0, 2.0 } };
        Tone noSweep;                                                 // bands on a switch
        noSweep.name = "switchy"; noSweep.reference = "0";
        noSweep.sections = { { SectionKind::Bell, 800.0, 1.0, -2.0, 2.0 } };
        st.tone = { both, noSweep };
        rig.chain = { st };
        const auto text = writeManifest (rig);
        bool v = false;
        const auto back = loadRigManifest (text, &v);
        ok (v && back.chain.size() == 1 && back.chain[0].tone.size() == 1,
            "both forms in the model → ONE in the pack (two would be refused); the sweepless knob is not written");
        ok (back.chain.size() == 1 && back.chain[0].tone.size() == 1
            && back.chain[0].tone[0].name == "both" && back.chain[0].tone[0].sections.size() == 1
            && back.chain[0].tone[0].positions.empty() && back.chain[0].tone[0].grid.points == 0,
            "…and the form written is `sections`, without the ladder's grid");
        ok (text.find ("switchy") == std::string::npos, "the sweepless knob left no trace");
    }

    // WRITER: a boosted file stamps boost=true; a rig with no boost control stamps no boost key.
    {
        Rig rig;
        Stage st;
        st.kind = StageKind::Nam;
        st.device.controls = parseControlsSpec ("boost:boost=off|on");
        ok (stampMeta (rig, st, { { "boost", "on" } }).at ("boost") == "true", "truthy boost stamps true");
        st.device.controls = parseControlsSpec ("gain:gain=0|150");
        ok (stampMeta (rig, st, { { "gain", "0" } }).count ("boost") == 0, "no boost control → no boost key");
    }

    std::printf (failures == 0 ? "ALL RIG TESTS PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
