// SPDX-License-Identifier: MIT
// Contract-first, adversarial tests for namz.h — try to BREAK the codec, not mirror it.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.

#define NAMZ_IMPLEMENTATION
#include "namz.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (! (cond)) { ++failures; std::fprintf (stderr, "FAIL: %s\n", msg); } } while (0)

static constexpr std::size_t kCap = 64u * 1024u * 1024u;

// A structurally faithful mini-.nam: nested submodel + metadata (null, unicode, big double) + non-weight
// numeric arrays that must NOT be packed.
static json makeNam (const std::vector<double>& top, const std::vector<double>& nested)
{
    json inner;
    inner["architecture"] = "WaveNet";
    inner["config"] = { { "kernel_sizes", { 6, 6, 15 } }, { "dilations", { 1, 3, 7 } } };
    inner["weights"] = nested;

    json j;
    j["version"] = "0.7.0";
    j["architecture"] = "SlimmableContainer";
    j["sample_rate"] = 48000;
    j["metadata"] = { { "loudness", -20.559617728394866 }, { "validation_esr", nullptr },
                      { "notes", "cafe snowman \"q\" \\ \n\t" } };
    j["config"]["submodels"] = json::array();
    j["config"]["submodels"].push_back ({ { "max_value", 0.5 }, { "model", inner } });
    j["weights"] = top;
    return j;
}

static json makeFlat (const std::vector<double>& w)
{
    json j; j["architecture"] = "WaveNet"; j["sample_rate"] = 48000; j["weights"] = w; return j;
}

// Collect every numeric "weights" array as float32, DFS order.
static void collect (const json& n, std::vector<std::vector<float>>& out)
{
    if (n.is_object())
        for (auto it = n.begin(); it != n.end(); ++it)
        {
            if (it.key() == "weights" && it.value().is_array())
            {
                std::vector<float> w; bool numeric = ! it.value().empty();
                for (const auto& x : it.value()) { if (! x.is_number()) { numeric = false; break; } w.push_back ((float) x.get<double>()); }
                if (numeric) out.push_back (w); else collect (it.value(), out);
            }
            else collect (it.value(), out);
        }
    else if (n.is_array())
        for (const auto& v : n) collect (v, out);
}
static std::vector<std::vector<float>> weightsOf (const json& j) { std::vector<std::vector<float>> w; collect (j, w); return w; }

static json unpackToJson (const std::vector<std::uint8_t>& blob)
{
    auto out = namz::unpack (blob.data(), blob.size(), kCap);
    if (out.empty()) return json();
    try { return json::parse (std::string ((const char*) out.data(), out.size())); }
    catch (...) { return json(); }
}

static bool weightsBitExact (const json& a, const json& b)
{
    auto wa = weightsOf (a), wb = weightsOf (b);
    if (wa.size() != wb.size()) return false;
    for (std::size_t i = 0; i < wa.size(); ++i)
    {
        if (wa[i].size() != wb[i].size()) return false;
        for (std::size_t k = 0; k < wa[i].size(); ++k)
        {
            // bit-exact float compare (handles -0 vs 0, and rejects any drift)
            std::uint32_t x, y; std::memcpy (&x, &wa[i][k], 4); std::memcpy (&y, &wb[i][k], 4);
            if (x != y) return false;
        }
    }
    return true;
}

static std::vector<std::uint8_t> packJson (const json& j, bool shuffle = true)
{
    auto s = j.dump();
    namz::PackOptions o; o.shuffle = shuffle;
    return namz::pack (s.data(), s.size(), o);
}

int main()
{
    // 1) round-trip bit-exact, both shuffle modes
    {
        auto nam = makeNam ({ -0.5, 0.5, 1.36406, -1.0e-6, 0.0, 0.333333333333 }, { 0.1, -0.25, 12.5, -7.0, 2.0e-7 });
        for (bool sh : { true, false })
        {
            auto p = packJson (nam, sh);
            CHECK (! p.empty(), "pack ok");
            CHECK (namz::isNamz (p.data(), p.size()), "has magic");
            auto back = unpackToJson (p);
            CHECK (! back.is_null(), "unpack ok");
            CHECK (weightsBitExact (nam, back), "weights bit-exact float32");
            CHECK (back["metadata"]["validation_esr"].is_null(), "null preserved");
            CHECK (back["config"]["submodels"][0]["model"]["config"]["dilations"]
                       == nam["config"]["submodels"][0]["model"]["config"]["dilations"], "non-weight int array untouched");
        }
    }

    // 2) numeric edges
    {
        const float fm = std::numeric_limits<float>::max();
        const float sub = std::numeric_limits<float>::denorm_min();
        std::vector<double> W = { 0.0, -0.0, (double) sub, (double) fm, -(double) fm, 0.1, 1.0 / 3.0, 1.0e-7 };
        auto nam = makeFlat (W);
        auto back = unpackToJson (packJson (nam, true));
        CHECK (! back.is_null(), "edges unpack ok");
        auto w = weightsOf (back)[0];
        CHECK (std::signbit (w[1]), "-0.0 keeps sign bit");
        CHECK (w[2] == sub, "smallest subnormal survives");
        CHECK (w[3] == fm, "FLT_MAX survives");
    }

    // 3) determinism + idempotence
    {
        auto nam = makeNam ({ 1, 2, 3 }, { 4, 5 });
        auto a = packJson (nam), b = packJson (nam);
        CHECK (a == b, "pack is byte-identical run-to-run");
        auto rebuilt = namz::unpack (a.data(), a.size(), kCap);
        namz::PackOptions o;
        auto c = namz::pack (rebuilt.data(), rebuilt.size(), o);
        CHECK (a == c, "pack.unpack.pack is a fixed point");
    }

    // 4) shuffle on vs off: same weights, size-neutral (store), both smaller than raw
    {
        std::vector<double> big (2000);
        for (std::size_t i = 0; i < big.size(); ++i) big[i] = std::sin ((double) i * 0.37) * 1.3;
        auto s = makeFlat (big).dump();
        auto on = packJson (makeFlat (big), true), off = packJson (makeFlat (big), false);
        CHECK (weightsBitExact (unpackToJson (on), unpackToJson (off)), "same weights either way");
        CHECK (on.size() == off.size(), "shuffle is size-neutral without a compressor");
        CHECK (on.size() < s.size() && off.size() < s.size(), "both smaller than raw JSON");
    }

    // 5) metadata header: typed --set, readMeta map, mirrored into the model
    {
        auto s = makeNam ({ 1 }, { 2 }).dump();
        namz::PackOptions o;
        o.metadata["tone_type"] = "hi-gain"; o.metadata["boost"] = "true";
        o.metadata["stages"] = "16"; o.metadata["device"] = "tube:1,pnp:1";
        // An identifier that happens to be spelled in digits: the zeros in front of it are part of it,
        // and a serial number that comes back 12345 is not the number stamped on the box.
        o.metadata["gear_serial_number"] = "0012345"; o.metadata["zero"] = "0";
        auto p = namz::pack (s.data(), s.size(), o);
        auto m = namz::readMeta (p.data(), p.size());
        CHECK (m["tone_type"] == "hi-gain", "meta string");
        CHECK (m["boost"] == "true", "meta bool -> text");
        CHECK (m["stages"] == "16", "meta int -> text");
        CHECK (m["device"] == "tube:1,pnp:1", "meta device");
        CHECK (m["gear_serial_number"] == "0012345", "a leading zero survives the header");
        CHECK (m["zero"] == "0", "bare zero is still a number");
        auto j = unpackToJson (p);
        CHECK (j["metadata"]["boost"].is_boolean() && j["metadata"]["boost"].get<bool>(), "boost typed bool in model");
        CHECK (j["metadata"]["stages"].get<int>() == 16, "stages typed int in model");
        CHECK (j["metadata"]["gear_serial_number"].is_string()
                   && j["metadata"]["gear_serial_number"].get<std::string>() == "0012345",
               "a leading zero stays a string in the model");
        CHECK (j["metadata"]["zero"].is_number_integer() && j["metadata"]["zero"].get<int>() == 0,
               "bare zero typed int in the model");
        auto raw = makeNam ({ 1 }, { 2 }).dump();
        CHECK (namz::readMeta (raw.data(), raw.size()).empty(), "readMeta on raw JSON is empty");
    }

    // 6) robustness: bad magic / version / codec / dtype rejected; truncation + trailing junk rejected
    {
        auto good = packJson (makeFlat ({ 0.5, -0.5, 0.25 }), true);
        auto flip = [&] (int idx, std::uint8_t v) { auto m = good; m[(std::size_t) idx] = v; return namz::unpack (m.data(), m.size(), kCap).empty(); };
        CHECK (flip (0, 'X'), "flipped magic rejected");
        CHECK (flip (4, 3), "formatVersion 3 rejected");
        CHECK (flip (5, 1), "codec 1 (deflate reserved) rejected");
        CHECK (flip (6, 1), "dtype 1 on a float32 payload rejected (its 12 bytes are 6 halves, the lengths say 3)");
        CHECK (flip (6, 2), "undefined dtype 2 rejected");
        CHECK (! namz::unpack (good.data(), good.size(), kCap).empty(), "control still unpacks");

        bool allEmptyOnTrunc = true;
        for (std::size_t len = 0; len < good.size(); ++len)
            allEmptyOnTrunc &= namz::unpack (good.data(), len, kCap).empty();
        CHECK (allEmptyOnTrunc, "every truncated prefix -> empty (store has no self-heal)");

        auto plus = good; plus.insert (plus.end(), { 'j', 'u', 'n', 'k' });
        CHECK (namz::unpack (plus.data(), plus.size(), kCap).empty(), "trailing junk rejected (exact-length check)");

        // zip/output cap: tiny cap rejects, generous cap accepts
        CHECK (namz::unpack (good.data(), good.size(), 8).empty(), "tiny output cap rejects");
        CHECK (! namz::unpack (good.data(), good.size(), kCap).empty(), "generous cap accepts");
    }

    // 7) structural edges: no weights, non-object top-level, non-numeric "weights" left alone
    {
        json noW; noW["architecture"] = "LSTM"; noW["sample_rate"] = 44100;
        auto s = noW.dump(); auto p = namz::pack (s.data(), s.size());
        CHECK (! p.empty() && unpackToJson (p) == noW, "a .nam with NO weights round-trips verbatim");

        for (const char* lit : { "{}", "[]", "[1,2,3]", "\"hi\"", "42", "null", "true" })
        {
            auto q = namz::pack (lit, std::strlen (lit));
            CHECK (! q.empty() && unpackToJson (q) == json::parse (lit), "degenerate top-level round-trips");
        }

        json odd; odd["weights"] = "not-an-array"; odd["also"] = json::object ({ { "weights", json::object() } });
        s = odd.dump(); p = namz::pack (s.data(), s.size());
        CHECK (! p.empty() && unpackToJson (p) == odd, "non-numeric \"weights\" survive verbatim");
    }

    // 8) decode — what a model builder gets: the skeleton parsed, each placeholder an index, the weights as
    //    float32 vectors that are the input's floats bit for bit; refilled, unpack's exact bytes.
    {
        const float fmax = std::numeric_limits<float>::max(), fmin = std::numeric_limits<float>::min();
        const float tiny = std::numeric_limits<float>::denorm_min();
        const std::vector<double> top { 0.1, -0.0, (double) fmax, (double) -fmin, (double) tiny, 1.0 / 3.0 };
        const std::vector<double> nested { -0.25, 0.7, (double) -tiny, 123456.789 };
        const json nam = makeNam (top, nested);
        auto bitsEqual = [] (const std::vector<float>* got, const std::vector<double>& want)
        {
            if (got == nullptr || got->size() != want.size()) return false;
            for (std::size_t i = 0; i < want.size(); ++i)
            {
                const float w = (float) want[i];
                if (std::memcmp (&(*got)[i], &w, 4) != 0) return false;
            }
            return true;
        };

        for (bool shuffle : { true, false })
        {
            const auto p = packJson (nam, shuffle);
            const auto d = namz::decode (p.data(), p.size(), kCap);
            CHECK (d.has_value(), "decode: a container decodes");
            if (! d) continue;
            const json& s = d->skeleton;
            CHECK (s["version"] == "0.7.0" && s["architecture"] == "SlimmableContainer" && s["sample_rate"] == 48000,
                   "decode: top-level version / architecture / sample_rate as the .nam had them");
            CHECK (s["metadata"] == nam["metadata"], "decode: metadata verbatim (null, unicode, big double)");
            CHECK (d->segments.size() == 2, "decode: one segment per weights array");
            CHECK (s["weights"].is_number_integer() && s["config"]["submodels"][0]["model"]["weights"].is_number_integer(),
                   "decode: each weights array is an integer placeholder in the skeleton");
            CHECK (bitsEqual (d->weightsOf (s), top), "decode: the top-level weights, bit for bit (-0, FLT_MAX, subnormal)");
            const json& sub = s["config"]["submodels"][0];
            CHECK (sub["max_value"] == 0.5 && sub["model"]["config"] == nam["config"]["submodels"][0]["model"]["config"],
                   "decode: a submodel's max_value and config as the .nam had them");
            CHECK (bitsEqual (d->weightsOf (sub["model"]), nested), "decode: a submodel's weights, bit for bit");
            CHECK (d->weightsOf (s["config"]) == nullptr && d->weightsOf (sub) == nullptr
                   && d->weightsOf (json (0)) == nullptr && d->weightsOf (json::object ({ { "weights", 0.5 } })) == nullptr,
                   "decode: weightsOf is nullptr where no placeholder sits");
            CHECK (d->weightsOf (json::object ({ { "weights", 2 } })) == nullptr
                   && d->weightsOf (json::object ({ { "weights", -1 } })) == nullptr,
                   "decode: weightsOf is nullptr for a placeholder naming no segment");

            json cfg = s["config"];
            CHECK (d->refill (cfg) && cfg == unpackToJson (p)["config"], "decode: refill of one subtree is unpack's subtree");
            json whole = s;
            CHECK (d->refill (whole), "decode: refill of the whole skeleton succeeds");
            const auto dumped = whole.dump();
            CHECK (std::vector<std::uint8_t> (dumped.begin(), dumped.end()) == namz::unpack (p.data(), p.size(), kCap),
                   "decode: refill + dump is unpack's exact bytes");
            json foreign = json::object ({ { "weights", 7 } });
            CHECK (! d->refill (foreign), "decode: refill refuses a placeholder naming no segment");
        }

        // the cap counts floats: exactly enough decodes, one short refuses; no weights needs no floats
        {
            const auto p = packJson (nam);
            const std::size_t total = top.size() + nested.size();
            CHECK (namz::decode (p.data(), p.size(), total).has_value(), "decode: maxFloats == the floats held decodes");
            CHECK (! namz::decode (p.data(), p.size(), total - 1).has_value(), "decode: maxFloats one short refuses");
            json noW; noW["architecture"] = "LSTM";
            const auto q = packJson (noW);
            const auto dq = namz::decode (q.data(), q.size(), 0);
            CHECK (dq.has_value() && dq->skeleton == noW && dq->segments.empty(), "decode: a .nam with NO weights needs no floats");
        }

        // degenerate top-levels decode to themselves, as unpack's do
        for (const char* lit : { "{}", "[]", "[1,2,3]", "\"hi\"", "42", "null", "true" })
        {
            const auto q = namz::pack (lit, std::strlen (lit));
            const auto dq = namz::decode (q.data(), q.size(), kCap);
            CHECK (dq.has_value() && dq->skeleton == json::parse (lit) && dq->segments.empty(), "decode: degenerate top-level decodes");
        }

        // an empty weights array is a segment of its own, empty
        {
            json e; e["architecture"] = "X"; e["weights"] = json::array();
            const auto q = packJson (e);
            const auto dq = namz::decode (q.data(), q.size(), kCap);
            CHECK (dq.has_value() && dq->segments.size() == 1 && dq->weightsOf (dq->skeleton) != nullptr
                   && dq->weightsOf (dq->skeleton)->empty(), "decode: an empty weights array is an empty segment");
        }
    }

    // 9) the version the header states is one version: the macros, the string, and the CMake project
    {
        const std::string fromMacros = std::to_string (NAMZ_VERSION_MAJOR) + "." + std::to_string (NAMZ_VERSION_MINOR)
                                     + "." + std::to_string (NAMZ_VERSION_PATCH);
        CHECK (fromMacros == NAMZ_VERSION, "version: NAMZ_VERSION_MAJOR/MINOR/PATCH spell NAMZ_VERSION");
#ifdef NAMZ_PROJECT_VERSION
        CHECK (std::string (NAMZ_PROJECT_VERSION) == NAMZ_VERSION, "version: NAMZ_VERSION is the CMake project's version");
#endif
    }

    // 10) the README's decode snippet, verbatim, runs on a plain model and on a container alike: it builds the
    //     plain model from its own weights and each submodel from its own, and never steps off the skeleton
    {
        json plain = makeFlat ({ 0.5, -0.25, 1.0 / 3.0 });
        plain["config"] = { { "layers", 2 } };
        const json container = makeNam ({ 1.5, 2.5 }, { -0.25, 0.7, 0.125 });

        std::vector<std::pair<json, std::vector<float>>> built;
        auto build = [&] (const nlohmann::json& config, const std::vector<float>& weights) { built.emplace_back (config, weights); };
        json refilledConfig;
        auto readme = [&] (const std::vector<std::uint8_t>& packed)
        {
            // ---- README.md, "Load a model without printing its weights" — verbatim ----
// the size arg is a zip-bomb cap in floats: what the file stores, and what its placeholders name
std::optional<namz::Decoded> d = namz::decode(packed.data(), packed.size(), 16u*1024u*1024u);
if (!d) return;                                         // refused: exactly what unpack refuses

const nlohmann::json& m = d->skeleton;                  // version, architecture, config, metadata, sample_rate
if (m.value("architecture", "") == "SlimmableContainer")
{
    for (const auto& sub : m.at("config").at("submodels"))   // each submodel: its own config and weights
        if (const std::vector<float>* w = d->weightsOf(sub.at("model")))
            build(sub.at("model").at("config"), *w);
}
else if (const std::vector<float>* w = d->weightsOf(m))     // a plain model; nullptr: it holds no weights
{
    build(m.at("config"), *w);
}

nlohmann::json cfg = m.at("config");                    // .at, not []: [] on a const json is UB for a missing key
d->refill(cfg);                                         // a subtree as .nam JSON, if a loader wants one
            // ---- end of the README snippet ----
            refilledConfig = cfg;
        };

        readme (packJson (plain));
        CHECK (built.size() == 1 && built[0].first == plain["config"] && built[0].second == std::vector<float> ({ 0.5f, -0.25f, (float) (1.0 / 3.0) })
               && refilledConfig == plain["config"], "README snippet: a plain model is built from its config and its weights");
        built.clear();
        const auto packedContainer = packJson (container);
        readme (packedContainer);
        CHECK (built.size() == 1 && built[0].first == container["config"]["submodels"][0]["model"]["config"]
               && built[0].second == std::vector<float> ({ -0.25f, 0.7f, 0.125f })
               && refilledConfig == unpackToJson (packedContainer)["config"], "README snippet: a container builds each submodel from its own config and weights");
    }

    std::fprintf (stderr, "\n==== namz tests: %d checks, %d failures — %s ====\n",
                  checks, failures, failures == 0 ? "ALL PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
