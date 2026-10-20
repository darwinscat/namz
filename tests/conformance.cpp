// SPDX-License-Identifier: MIT
// The reference consumer of the conformance vectors (conformance/). Every language port runs the same
// manifest: encode a valid input with the given options -> BYTE-MATCH the .namz; decode -> weights
// bit-exact + idempotent; every invalid .namz -> rejected (empty), never a crash. The C++ reference also
// holds its second reader, namz::decode, to the same vectors: its segments are the input's weights bit
// for bit, refilled it dumps to unpack's exact bytes, and it refuses every invalid blob unpack refuses.
// Every decodeOnly .namz must unpack (and decode -> refill -> dump) to its .nam byte for byte; every
// invalidNam .nam must be refused by pack.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.

#define NAMZ_IMPLEMENTATION
#include "namz.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;
using bytes = std::vector<std::uint8_t>;

#ifndef NAMZ_CONFORMANCE_DIR
#error "NAMZ_CONFORMANCE_DIR must be defined (path to the conformance/ directory)"
#endif

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (! (cond)) { ++failures; std::fprintf (stderr, "FAIL: %s\n", msg); } } while (0)

static constexpr std::size_t kCap = 256u * 1024u * 1024u;
static constexpr std::size_t kMaxFloats = 64u * 1024u * 1024u;

// Every numeric "weights" array of a .nam as float32, in the order pack numbers them (DFS over nlohmann's
// sorted keys; an empty array is a segment too).
static void collectWeights (const json& node, std::vector<std::vector<float>>& out)
{
    if (node.is_object())
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            const auto& v = it.value();
            bool numeric = it.key() == "weights" && v.is_array();
            if (numeric)
                for (const auto& x : v)
                    if (! x.is_number()) { numeric = false; break; }
            if (numeric)
            {
                std::vector<float> w;
                for (const auto& x : v) w.push_back ((float) x.get<double>());
                out.push_back (std::move (w));
            }
            else
                collectWeights (v, out);
        }
    else if (node.is_array())
        for (const auto& v : node) collectWeights (v, out);
}

static bool sameBits (const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].size() != b[i].size()
            || (! a[i].empty() && std::memcmp (a[i].data(), b[i].data(), a[i].size() * sizeof (float)) != 0))
            return false;
    return true;
}

static bytes readFile (const std::string& path, bool& ok)
{
    std::ifstream f (path, std::ios::binary);
    if (! f) { ok = false; return {}; }
    bytes d ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    ok = true;
    return d;
}

int main()
{
    const std::string dir = NAMZ_CONFORMANCE_DIR;
    bool ok = false;
    auto manifestBytes = readFile (dir + "/manifest.json", ok);
    if (! ok) { std::fprintf (stderr, "cannot read manifest at %s/manifest.json\n", dir.c_str()); return 1; }
    json manifest = json::parse (std::string ((const char*) manifestBytes.data(), manifestBytes.size()));

    for (const auto& c : manifest["valid"])
    {
        const std::string name = c["name"].get<std::string>();
        bool a = false, b = false;
        auto input    = readFile (dir + "/" + c["input"].get<std::string>(), a);
        auto expected = readFile (dir + "/" + c["output"].get<std::string>(), b);
        CHECK (a && b, ("valid: files present for " + name).c_str());
        if (! (a && b)) continue;

        namz::PackOptions opts;
        opts.shuffle = c.value ("shuffle", true);
        if (c.contains ("set"))
            for (auto it = c["set"].begin(); it != c["set"].end(); ++it)
                opts.metadata[it.key()] = it.value().get<std::string>();

        auto encoded = namz::pack (input.data(), input.size(), opts);
        CHECK (encoded == expected, ("valid: encode byte-matches expected for " + name).c_str());

        auto decoded = namz::unpack (expected.data(), expected.size(), kCap);
        CHECK (! decoded.empty(), ("valid: decode non-empty for " + name).c_str());

        // idempotence: re-encoding the decode reproduces the reference bytes exactly.
        auto re = namz::pack (decoded.data(), decoded.size(), opts);
        CHECK (re == expected, ("valid: decode->encode is a fixed point for " + name).c_str());

        // namz::decode — the same file, no weight ever made text.
        auto d = namz::decode (expected.data(), expected.size(), kMaxFloats);
        CHECK (d.has_value(), ("valid: namz::decode accepts " + name).c_str());
        if (! d) continue;
        std::vector<std::vector<float>> want;
        collectWeights (json::parse (input.begin(), input.end()), want);
        CHECK (sameBits (d->segments, want), ("valid: namz::decode segments are the input's float32 weights, bit for bit, for " + name).c_str());
        json full = d->skeleton;
        CHECK (d->refill (full), ("valid: namz::decode refills its whole skeleton for " + name).c_str());
        const auto dumped = full.dump();
        CHECK (bytes (dumped.begin(), dumped.end()) == decoded, ("valid: namz::decode -> refill -> dump is unpack's exact bytes for " + name).c_str());
    }

    for (const auto& c : manifest["invalid"])
    {
        const std::string name = c["name"].get<std::string>();
        bool a = false;
        auto blob = readFile (dir + "/" + c["file"].get<std::string>(), a);
        CHECK (a, ("invalid: file present for " + name).c_str());
        // must be rejected cleanly (empty) and must not crash reaching here.
        CHECK (namz::unpack (blob.data(), blob.size(), kCap).empty(), ("invalid: rejected -> " + name).c_str());
        CHECK (! namz::decode (blob.data(), blob.size(), kMaxFloats).has_value(), ("invalid: namz::decode rejects -> " + name).c_str());
    }

    CHECK (manifest.contains ("decodeOnly") && manifest.contains ("invalidNam"), "manifest: decodeOnly and invalidNam present");

    // files pack never writes — a hand-made placeholder, a v1 container — and the exact .nam they decode to
    for (const auto& c : manifest["decodeOnly"])
    {
        const std::string name = c["name"].get<std::string>();
        bool a = false, b = false;
        auto blob = readFile (dir + "/" + c["file"].get<std::string>(), a);
        auto want = readFile (dir + "/" + c["output"].get<std::string>(), b);
        CHECK (a && b, ("decodeOnly: files present for " + name).c_str());
        if (! (a && b)) continue;
        CHECK (namz::unpack (blob.data(), blob.size(), kCap) == want, ("decodeOnly: unpack byte-matches the .nam for " + name).c_str());
        auto d = namz::decode (blob.data(), blob.size(), kMaxFloats);
        CHECK (d.has_value(), ("decodeOnly: namz::decode accepts " + name).c_str());
        if (! d) continue;
        json full = d->skeleton;
        const bool filled = d->refill (full);
        const auto dumped = full.dump();
        CHECK (filled && bytes (dumped.begin(), dumped.end()) == want, ("decodeOnly: namz::decode -> refill -> dump byte-matches the .nam for " + name).c_str());
    }

    // a .nam pack must refuse
    for (const auto& c : manifest["invalidNam"])
    {
        const std::string name = c["name"].get<std::string>();
        bool a = false;
        auto input = readFile (dir + "/" + c["file"].get<std::string>(), a);
        CHECK (a, ("invalidNam: file present for " + name).c_str());
        CHECK (namz::pack (input.data(), input.size()).empty(), ("invalidNam: pack refuses -> " + name).c_str());
    }

    std::fprintf (stderr, "\n==== namz conformance: %d checks, %d failures — %s ====\n",
                  checks, failures, failures == 0 ? "ALL PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
