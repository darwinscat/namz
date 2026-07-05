// SPDX-License-Identifier: MIT
// The reference consumer of the conformance vectors (conformance/). Every language port runs the same
// manifest: encode a valid input with the given options -> BYTE-MATCH the .namz; decode -> weights
// bit-exact + idempotent; every invalid .namz -> rejected (empty), never a crash.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.

#define NAMZ_IMPLEMENTATION
#include "namz.h"

#include <nlohmann/json.hpp>

#include <cstdio>
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
    }

    for (const auto& c : manifest["invalid"])
    {
        const std::string name = c["name"].get<std::string>();
        bool a = false;
        auto blob = readFile (dir + "/" + c["file"].get<std::string>(), a);
        CHECK (a, ("invalid: file present for " + name).c_str());
        // must be rejected cleanly (empty) and must not crash reaching here.
        CHECK (namz::unpack (blob.data(), blob.size(), kCap).empty(), ("invalid: rejected -> " + name).c_str());
    }

    std::fprintf (stderr, "\n==== namz conformance: %d checks, %d failures — %s ====\n",
                  checks, failures, failures == 0 ? "ALL PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
