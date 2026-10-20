// SPDX-License-Identifier: MIT
// namz — the float16 dtype (`.nam16z`): what pack writes when asked for DtypeF16, and that the readers widen
// it back. Every half is checked against a reference rounding that shares no code with namz's — arithmetic
// on doubles (frexp, nearbyint) instead of bits — and against a table of numpy.float16 results; the bytes
// against goldens written by hand from NAMZ-FORMAT.md. Float32 stays the default and is not touched here:
// its goldens live in adversarial.cpp and conformance/.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.

#define NAMZ_IMPLEMENTATION
#include "namz.h"

#include <nlohmann/json.hpp>

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using json = nlohmann::json;
using bytes = std::vector<std::uint8_t>;

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (! (cond)) { ++failures; std::fprintf (stderr, "FAIL: %s\n", msg); } } while (0)

static constexpr std::size_t kCap = 256u * 1024u * 1024u;
static constexpr std::size_t kMaxFloats = 64u * 1024u * 1024u;

static bytes fromHex (const char* h)
{
    auto isHex = [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    bytes out;
    int hi = -1;
    for (const char* p = h; *p; ++p)
    {
        if (! isHex (*p)) continue;
        const int v = *p <= '9' ? *p - '0' : *p - 'a' + 10;
        if (hi < 0) hi = v;
        else { out.push_back ((std::uint8_t) ((hi << 4) | v)); hi = -1; }
    }
    return out;
}

static bytes packAs (const json& j, namz::Dtype dtype, bool shuffle = true)
{
    const auto s = j.dump();
    namz::PackOptions o; o.shuffle = shuffle; o.dtype = dtype;
    return namz::pack (s.data(), s.size(), o);
}

static std::uint32_t bitsOf (float f) { std::uint32_t b; std::memcpy (&b, &f, 4); return b; }

// The reference: the half nearest the float32 `f`, ties to even, as a double — by arithmetic, sharing nothing
// with namz's bit code. A half has 11 significant bits, its step at |f| in [2^(e-1), 2^e) is 2^(e-11), never
// finer than the subnormal step 2^-24; scaling by a power of two is exact, nearbyint rounds ties to even in
// the default mode. False where half has no such number (non-finite, or past 65504).
static bool refHalf (float f, double& r)
{
    if (! std::isfinite (f)) return false;
    const double v = f;
    if (v == 0.0) { r = v; return true; }
    int e = 0;
    std::frexp (std::fabs (v), &e);
    const int q = e - 11 > -24 ? e - 11 : -24;
    r = std::ldexp (std::nearbyint (std::ldexp (v, -q)), q);
    return std::fabs (r) <= 65504.0;
}

// The float32 weights `w` as a `.nam`, packed F16 (both payload layouts) and read back by both readers:
// every weight must come back as exactly refHalf of its float32, sign of zero included.
static bool matchesReference (const std::vector<float>& w, const char* what)
{
    json j; j["architecture"] = "WaveNet";
    j["weights"] = json::array();
    for (const float f : w) j["weights"].push_back ((double) f);
    bool ok = true;
    for (bool shuffle : { true, false })
    {
        const auto p = packAs (j, namz::DtypeF16, shuffle);
        const auto d = namz::decode (p.data(), p.size(), kMaxFloats);
        const auto u = namz::unpack (p.data(), p.size(), kCap);
        if (p.empty() || ! d || u.empty() || d->segments.size() != 1 || d->segments[0].size() != w.size())
        {
            std::fprintf (stderr, "  %s: pack/read failed (shuffle %d)\n", what, (int) shuffle);
            return false;
        }
        const auto back = json::parse (std::string ((const char*) u.data(), u.size()))["weights"];
        int reported = 0;
        for (std::size_t i = 0; i < w.size(); ++i)
        {
            double r = 0.0;
            const bool has = refHalf (w[i], r);
            const float got = d->segments[0][i];
            const float printed = (float) back[i].get<double>();
            if (! has || bitsOf (got) != bitsOf ((float) r) || bitsOf (printed) != bitsOf ((float) r))
            {
                ok = false;
                if (reported++ < 5)
                    std::fprintf (stderr, "  %s: weight %.9g -> decode %.9g, unpack %.9g, reference %.9g\n",
                                  what, (double) w[i], (double) got, (double) printed, r);
            }
        }
    }
    return ok;
}

int main()
{
    std::fesetround (FE_TONEAREST);   // what refHalf's nearbyint rounds by

    // === 1) GOLDEN BYTES — written by hand from the format: dtype byte 1, 2 bytes a weight, little-endian,
    //        the shuffled payload in two planes (every low byte, then every high byte). 0.5 = 0x3800,
    //        -0.5 = 0xB800, 0.25 = 0x3400. The skeleton is the float32 file's, byte for byte.
    {
        json j; j["architecture"] = "WaveNet"; j["weights"] = std::vector<double> { 0.5, -0.5, 0.25 };
        const char* skeleton = "26000000 7b22617263686974656374757265223a22576176654e6574222c2277656967687473223a307d"
                               "01000000 03000000";
        const bytes shuffled   = fromHex ((std::string ("4e414d5a 02 00 01 01 0000 ") + skeleton + " 000000 38b834").c_str());
        const bytes interleave = fromHex ((std::string ("4e414d5a 02 00 01 00 0000 ") + skeleton + " 0038 00b8 0034").c_str());
        const auto ps = packAs (j, namz::DtypeF16, true);
        const auto pi = packAs (j, namz::DtypeF16, false);
        CHECK (ps == shuffled && ps.size() == 66, "golden: F16 shuffled pack is the hand-written bytes (2 planes)");
        CHECK (pi == interleave, "golden: F16 unshuffled pack is the hand-written bytes (little-endian halves)");
        const auto d = namz::decode (shuffled.data(), shuffled.size(), kMaxFloats);
        CHECK (d && d->segments.size() == 1 && d->segments[0] == std::vector<float> ({ 0.5f, -0.5f, 0.25f }),
               "golden: decode widens the hand-written F16 file to its float32 weights");
        const auto u = namz::unpack (interleave.data(), interleave.size(), kCap);
        CHECK (std::string (u.begin(), u.end()) == "{\"architecture\":\"WaveNet\",\"weights\":[0.5,-0.5,0.25]}",
               "golden: unpack prints the hand-written F16 file's weights as float32 numbers");
        const auto f32 = packAs (j, namz::DtypeF32, true);
        CHECK (f32.size() == 72 && bytes (f32.begin() + 7, f32.end() - 12) == bytes (ps.begin() + 7, ps.end() - 6)
               && f32[6] == 0 && ps[6] == 1,
               "golden: the F16 file differs from the F32 one in the dtype byte and the payload only");
    }

    // === 2) THE HALF BITS — numpy.float16 of each float32 (numpy 2.5.4), read straight off the payload ===
    {
        struct Row { float f; std::uint16_t half; };
        const Row rows[] = {
            { 1.0f / 3.0f, 0x3555 }, { 0.1f, 0x2E66 }, { 3.14159274f, 0x4248 }, { -1.5f, 0xBE00 },
            { 65504.0f, 0x7BFF }, { 65519.9961f, 0x7BFF },                 // up to the overflow tie: 65504
            { 2049.0f, 0x6800 }, { 2051.0f, 0x6802 },                      // ties to even, both ways
            { 6.103515625e-05f, 0x0400 },                                  // 2^-14, the smallest normal
            { 6.1005353927612305e-05f, 0x0400 },                           // tie between 0x3FF and 0x400: even
            { 5.9604644775390625e-08f, 0x0001 },                           // 2^-24, the smallest subnormal
            { 2.9802322387695312e-08f, 0x0000 },                           // 2^-25: tie, to even zero
            { -2.9802322387695312e-08f, 0x8000 },                          // -2^-25: tie, to the zero of its sign
            { 2.98023259404089e-08f, 0x0001 },                             // just above it
            { 8.940696716308594e-08f, 0x0002 },                            // 1.5 subnormal steps: tie, to 2
            { 1e-8f, 0x0000 }, { -1e-8f, 0x8000 }, { -0.0f, 0x8000 }, { 0.0f, 0x0000 } };
        json j; j["architecture"] = "X"; j["weights"] = json::array();
        for (const auto& r : rows) j["weights"].push_back ((double) r.f);
        const auto p = packAs (j, namz::DtypeF16, false);
        const std::size_t n = sizeof (rows) / sizeof (rows[0]);
        bool ok = p.size() > 2 * n;
        for (std::size_t i = 0; ok && i < n; ++i)
        {
            const std::size_t at = p.size() - 2 * n + 2 * i;
            const auto h = (std::uint16_t) (p[at] | (p[at + 1] << 8));
            if (h != rows[i].half) { ok = false; std::fprintf (stderr, "  row %zu: %.9g -> 0x%04X, numpy 0x%04X\n", i, (double) rows[i].f, h, rows[i].half); }
        }
        CHECK (ok, "bits: every half is numpy.float16's, ties to even, subnormals kept, signed zero kept");
        std::vector<float> w;
        for (const auto& r : rows) w.push_back (r.f);
        CHECK (matchesReference (w, "table"), "bits: the table agrees with the reference rounding too");
    }

    // === 3) EVERY HALF, AND EVERY TIE BETWEEN TWO — each finite half, the float32 exactly halfway to the next
    //        one, and the float32s either side of that midpoint, both signs: 253,946 weights ===
    {
        std::vector<float> w;
        auto halfValue = [] (std::uint32_t h) {   // by the definition, not by bits: m * 2^-24, or (1024 + m) * 2^(e - 25)
            const int e = (int) (h >> 10), m = (int) (h & 0x3FF);
            return e == 0 ? std::ldexp ((double) m, -24) : std::ldexp ((double) (1024 + m), e - 25);
        };
        for (std::uint32_t h = 0; h < 0x7BFF; ++h)   // the tie above 0x7BFF overflows: section 4
        {
            const double v = halfValue (h), next = halfValue (h + 1);
            const float mid = (float) ((v + next) / 2);   // 12 significant bits: exact in a float32
            for (const float f : { (float) v, mid, std::nextafter (mid, 0.0f), std::nextafter (mid, 1e30f) })
            {
                w.push_back (f);
                w.push_back (-f);
            }
        }
        w.push_back (65504.0f); w.push_back (-65504.0f);
        CHECK (matchesReference (w, "every half"), "every half: each half, each tie, each neighbour of a tie -> the reference half");
    }

    // === 4) RANDOM float32 — 200,000 within half's range: one in four any bit pattern (f32 subnormals and
    //        the tiny ones that round to zero), three in four with an exponent where halves live, 2^-29 .. 2^15 ===
    {
        std::mt19937_64 rng (0xF16F16u);
        std::vector<float> w;
        while (w.size() < 200000)
        {
            auto bits = (std::uint32_t) rng();
            if (w.size() % 4 != 0)
                bits = (bits & 0x807FFFFFu) | ((std::uint32_t) (98 + rng() % 45) << 23);
            float f; std::memcpy (&f, &bits, 4);
            double r;
            if (refHalf (f, r)) w.push_back (f);
        }
        CHECK (matchesReference (w, "random"), "random: 200k float32 -> the reference half, bit for bit");
    }

    // === 5) OVERFLOW AND NON-FINITE — the pack is refused, nothing is written ===
    {
        auto refused = [] (const json& weights) {
            json j; j["architecture"] = "X"; j["weights"] = weights;
            return packAs (j, namz::DtypeF16, true).empty() && packAs (j, namz::DtypeF16, false).empty();
        };
        CHECK (refused ({ 0.5, 65520.0 }) && refused ({ -65520.0 }) && refused ({ 1e5 }) && refused ({ 3.4028234663852886e38 }),
               "overflow: a weight at or past the overflow tie (|w| >= 65520) refuses the F16 pack");
        CHECK (refused ({ 1e39 }) && refused ({ -1e39 }), "overflow: a weight whose float32 is not finite refuses the F16 pack");
        {
            json inner; inner["architecture"] = "Linear"; inner["weights"] = { 0.25, 70000.0 };
            json c; c["architecture"] = "SlimmableContainer"; c["weights"] = { 1.0 };
            c["config"]["submodels"] = json::array ({ { { "max_value", 1.0 }, { "model", inner } } });
            CHECK (packAs (c, namz::DtypeF16).empty() && ! packAs (c, namz::DtypeF32).empty(),
                   "overflow: one weight past half's range in a nested submodel refuses the whole F16 pack; F32 packs it");
        }
        json j; j["architecture"] = "X"; j["weights"] = { 65519.99609375, -65504.0 };
        const auto p = packAs (j, namz::DtypeF16);
        const auto d = namz::decode (p.data(), p.size(), kMaxFloats);
        CHECK (d && d->segments[0] == std::vector<float> ({ 65504.0f, -65504.0f }), "overflow: just under the tie packs, as 65504");
        namz::PackOptions bad; bad.dtype = (namz::Dtype) 2;
        const std::string s = j.dump();
        CHECK (namz::pack (s.data(), s.size(), bad).empty(), "overflow: pack refuses a dtype namz does not define");
    }

    // === 6) ROUND TRIP — a real-shaped container: both layouts read back alike; F16 is idempotent ===
    {
        json inner; inner["architecture"] = "WaveNet"; inner["config"] = { { "layers", 3 } };
        inner["weights"] = std::vector<double> { 0.5, -0.25, 3.0e-39, -0.0, 0.123456789, -7.75e-6 };
        json c; c["architecture"] = "SlimmableContainer"; c["version"] = "0.7.0";
        c["config"]["submodels"] = json::array ({ { { "max_value", 1.0 }, { "model", inner } } });
        c["metadata"] = { { "name", "Kraken" }, { "gain", 90 } };
        c["weights"] = std::vector<double> { 1.5, 2.5, -3.5, 1e-4 };
        const auto s = packAs (c, namz::DtypeF16, true), i = packAs (c, namz::DtypeF16, false);
        const auto us = namz::unpack (s.data(), s.size(), kCap), ui = namz::unpack (i.data(), i.size(), kCap);
        CHECK (! us.empty() && us == ui, "round trip: shuffled and unshuffled F16 read back to the same .nam");
        const auto again = namz::pack (us.data(), us.size(), [] { namz::PackOptions o; o.dtype = namz::DtypeF16; return o; }());
        CHECK (again == s, "round trip: pack F16 of the unpacked F16 file is the same file (idempotent)");
        const auto d = namz::decode (s.data(), s.size(), kMaxFloats);
        json full = d ? d->skeleton : json();
        CHECK (d && d->refill (full) && full.dump() == std::string (us.begin(), us.end()),
               "round trip: decode refilled and dumped is unpack's output, byte for byte");
        // a float32 file's weights, re-packed as F16, give the same file as packing the .nam as F16
        const auto f32 = packAs (c, namz::DtypeF32);
        const auto u32 = namz::unpack (f32.data(), f32.size(), kCap);
        namz::PackOptions o16; o16.dtype = namz::DtypeF16;
        CHECK (namz::pack (u32.data(), u32.size(), o16) == s, "round trip: .nam -> .namz -> .nam16z is .nam -> .nam16z");
        namz::PackOptions o32;
        const auto cs = c.dump();
        CHECK (namz::pack (cs.data(), cs.size(), o32) == f32 && f32[6] == 0, "round trip: the default dtype is F32");
    }

    std::fprintf (stderr, "%s — %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures ? 1 : 0;
}
