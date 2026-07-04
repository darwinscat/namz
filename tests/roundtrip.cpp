// SPDX-License-Identifier: MIT
// Contract-first, adversarial tests for namz.h — try to BREAK the codec, not mirror it.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.

#define NAMZ_IMPLEMENTATION
#include "namz.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
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
        auto p = namz::pack (s.data(), s.size(), o);
        auto m = namz::readMeta (p.data(), p.size());
        CHECK (m["tone_type"] == "hi-gain", "meta string");
        CHECK (m["boost"] == "true", "meta bool -> text");
        CHECK (m["stages"] == "16", "meta int -> text");
        CHECK (m["device"] == "tube:1,pnp:1", "meta device");
        auto j = unpackToJson (p);
        CHECK (j["metadata"]["boost"].is_boolean() && j["metadata"]["boost"].get<bool>(), "boost typed bool in model");
        CHECK (j["metadata"]["stages"].get<int>() == 16, "stages typed int in model");
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
        CHECK (flip (6, 1), "dtype 1 (f16) rejected");
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

    std::fprintf (stderr, "\n==== namz tests: %d checks, %d failures — %s ====\n",
                  checks, failures, failures == 0 ? "ALL PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
