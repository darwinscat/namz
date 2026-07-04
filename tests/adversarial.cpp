// SPDX-License-Identifier: MIT
// namz — MANIACAL, cross-platform-hostile tests. Golden bytes, explicit little-endian invariants, the
// "no trailing slack" guard (the MSVC bug class), random float32 fuzz, bit-flip fuzz, hand-crafted
// malicious containers, v1 back-compat, deep nesting, a unicode zoo, a comma-decimal locale, oversized
// metadata. If any platform diverges by a single byte, something here fails.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.

#define NAMZ_IMPLEMENTATION
#include "namz.h"

#include <nlohmann/json.hpp>

#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using json = nlohmann::json;
using bytes = std::vector<std::uint8_t>;

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (! (cond)) { ++failures; std::fprintf (stderr, "FAIL: %s\n", msg); } } while (0)

static constexpr std::size_t kCap = 256u * 1024u * 1024u;

static bytes fromHex (const char* h)
{
    auto isHex = [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    auto nib   = [] (char c) -> int { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    bytes out;
    int hi = -1;
    for (const char* p = h; *p; ++p)   // skip any non-hex (spaces/newlines) so the literal can wrap freely
    {
        if (! isHex (*p)) continue;
        if (hi < 0) hi = nib (*p);
        else { out.push_back ((std::uint8_t) ((hi << 4) | nib (*p))); hi = -1; }
    }
    return out;
}

static bytes packJson (const json& j, bool shuffle = true)
{
    auto s = j.dump();
    namz::PackOptions o; o.shuffle = shuffle;
    return namz::pack (s.data(), s.size(), o);
}

static json unpackToJson (const bytes& blob)
{
    auto out = namz::unpack (blob.data(), blob.size(), kCap);
    if (out.empty()) return json();
    try { return json::parse (std::string ((const char*) out.data(), out.size())); }
    catch (...) { return json(); }
}

static std::uint32_t rdU32 (const bytes& b, std::size_t off)
{
    return (std::uint32_t) b[off] | ((std::uint32_t) b[off + 1] << 8)
         | ((std::uint32_t) b[off + 2] << 16) | ((std::uint32_t) b[off + 3] << 24);
}

int main()
{
    // === 1) GOLDEN BYTES — locks determinism, endianness, and nlohmann serialization across platforms ===
    {
        // pack({"architecture":"WaveNet","weights":[0.5,-0.5,0.25]}) with shuffle — captured on reference.
        const bytes golden = fromHex (
            "4e414d5a020000010000260000007b22617263686974656374757265223a22576176654e6574222c227765"
            "69676874 73223a307d01000000030000000000000000000000803fbf3e");
        json j; j["architecture"] = "WaveNet"; j["weights"] = std::vector<double> { 0.5, -0.5, 0.25 };
        auto p = packJson (j, true);
        CHECK (p == golden, "golden: pack is byte-identical to the reference vector");
        CHECK (p.size() == 72, "golden: exact size (no trailing slack)");
        CHECK (! unpackToJson (p).is_null(), "golden: round-trips");
    }

    // === 2) explicit LITTLE-ENDIAN header/length fields (skeletonLen > 255 -> high byte must be set) ===
    {
        json j; j["architecture"] = std::string (600, 'x'); j["weights"] = std::vector<double> { 1.0 };
        auto p = packJson (j, false);
        CHECK (p.size() > 10, "LE: packed");
        // body starts at 10 (metaLen 0). first field is u32 skeletonLen, little-endian.
        const std::uint32_t skLen = rdU32 (p, 10);
        CHECK (skLen > 255, "LE: skeletonLen genuinely exceeds one byte (exercises byte[1]+)");
        CHECK (p[11] != 0, "LE: skeletonLen high byte is where LE puts it, not byte[0]");
        // numArrays sits right after the skeleton and must read back as 1, little-endian.
        CHECK (rdU32 (p, 14 + skLen) == 1, "LE: numArrays reads back as 1");
    }

    // === 3) NO TRAILING SLACK — appending or dropping a single byte must break the exact-length check ===
    {
        for (int n : { 1, 7, 33, 250 })
        {
            std::vector<double> w (n);
            for (int i = 0; i < n; ++i) w[i] = std::sin (i * 0.7);
            json j; j["architecture"] = "X"; j["weights"] = w;
            auto p = packJson (j, true);
            CHECK (! unpackToJson (p).is_null(), "slack: exact blob round-trips");
            auto plus = p; plus.push_back (0);
            CHECK (namz::unpack (plus.data(), plus.size(), kCap).empty(), "slack: +1 byte rejected");
            CHECK (namz::unpack (p.data(), p.size() - 1, kCap).empty(), "slack: -1 byte rejected");
        }
    }

    // === 4) RANDOM float32 FUZZ — 100k finite floats, round-trip must be bit-exact ===
    {
        std::mt19937_64 rng (0xC0FFEEu);
        std::vector<double> w; w.reserve (100000);
        std::vector<float> expect; expect.reserve (100000);
        while (w.size() < 100000)
        {
            std::uint32_t bits = (std::uint32_t) rng();
            std::uint8_t exp = (std::uint8_t) ((bits >> 23) & 0xFF);
            if (exp == 0xFF) continue;                       // skip NaN/Inf (out of contract)
            float f; std::memcpy (&f, &bits, 4);
            expect.push_back (f);
            w.push_back ((double) f);
        }
        json j; j["architecture"] = "WaveNet"; j["weights"] = w;
        auto back = unpackToJson (packJson (j, true));
        bool exact = ! back.is_null() && back["weights"].size() == expect.size();
        if (exact)
            for (std::size_t i = 0; i < expect.size(); ++i)
            {
                float got = (float) back["weights"][i].get<double>();
                std::uint32_t a, b; std::memcpy (&a, &got, 4); std::memcpy (&b, &expect[i], 4);
                if (a != b) { exact = false; break; }
            }
        CHECK (exact, "fuzz: 100k random float32 round-trip bit-exact");
    }

    // === 5) BIT-FLIP FUZZ — arbitrary corruption must never crash/hang; only empty or a valid re-pack ===
    {
        auto good = packJson ([] { json j; j["architecture"] = "WaveNet";
                                   j["weights"] = std::vector<double> { 0.1, -0.2, 0.3, 0.4, -0.5 }; return j; }(), true);
        std::mt19937_64 rng (0xBADF00Du);
        bool safe = true;
        for (int iter = 0; iter < 8000 && safe; ++iter)
        {
            auto m = good;
            const int flips = 1 + (int) (rng() % 3);
            for (int f = 0; f < flips; ++f)
            {
                std::size_t pos = (std::size_t) (rng() % m.size());
                m[pos] ^= (std::uint8_t) (1u << (rng() % 8));
            }
            auto out = namz::unpack (m.data(), m.size(), kCap);   // must return without crashing
            if (! out.empty())
            {
                // if it decoded to something, it must be parseable and re-packable (no garbage escapes)
                try { auto jj = json::parse (std::string ((const char*) out.data(), out.size()));
                      auto re = namz::pack (out.data(), out.size()); (void) re; if (jj.is_discarded()) safe = false; }
                catch (...) { safe = false; }
            }
        }
        CHECK (safe, "bitflip: 8000 corruptions -> no crash, no garbage escapes (empty or valid JSON)");
    }

    // === 6) HAND-CRAFTED MALICIOUS containers — lying lengths / counts / metaLen must be rejected ===
    {
        auto base = packJson ([] { json j; j["architecture"] = "X"; j["weights"] = std::vector<double> { 1, 2, 3 }; return j; }(), true);

        auto corrupt = [&] (std::size_t off, std::uint32_t v) { auto m = base; if (off + 4 <= m.size()) { m[off] = v & 0xff; m[off + 1] = (v >> 8) & 0xff; m[off + 2] = (v >> 16) & 0xff; m[off + 3] = (v >> 24) & 0xff; } return namz::unpack (m.data(), m.size(), kCap).empty(); };
        CHECK (corrupt (10, 0xFFFFFFFFu), "crafted: absurd skeletonLen rejected");
        CHECK (corrupt (10, 0u) || true, "crafted: zero skeletonLen handled (no crash)");
        // numArrays sits after the skeleton; blow it up -> OOM guard must reject.
        const std::uint32_t skLen = rdU32 (base, 10);
        CHECK (corrupt (14 + skLen, 0xFFFFFFFFu), "crafted: absurd numArrays rejected (OOM guard)");
        CHECK (corrupt (14 + skLen, 99u), "crafted: numArrays that overruns the buffer rejected");
        // a lying metaLen (v2 header, bytes 8..9) larger than the buffer.
        { auto m = base; m[8] = 0xff; m[9] = 0xff; CHECK (namz::unpack (m.data(), m.size(), kCap).empty(), "crafted: metaLen > buffer rejected"); }

        // every version/codec/dtype byte value: only (ver<=2, codec 0, dtype 0) may decode.
        bool onlyValidAccepted = true;
        for (int v = 0; v < 256; ++v)
        {
            auto m = base; m[4] = (std::uint8_t) v;
            bool ok = ! namz::unpack (m.data(), m.size(), kCap).empty();
            if (ok && v > 2) onlyValidAccepted = false;            // future version must be refused
        }
        for (int c = 1; c < 256; ++c) { auto m = base; m[5] = (std::uint8_t) c; if (! namz::unpack (m.data(), m.size(), kCap).empty()) onlyValidAccepted = false; }
        for (int d = 1; d < 256; ++d) { auto m = base; m[6] = (std::uint8_t) d; if (! namz::unpack (m.data(), m.size(), kCap).empty()) onlyValidAccepted = false; }
        CHECK (onlyValidAccepted, "crafted: unknown version/codec/dtype all refused");

        // tiny / degenerate buffers never crash
        for (std::size_t len = 0; len <= base.size(); ++len)
            (void) namz::unpack (base.data(), len, kCap);
        CHECK (namz::unpack (nullptr, 0, kCap).empty(), "crafted: null buffer -> empty");
        CHECK (namz::unpack (base.data(), 4, kCap).empty(), "crafted: magic-only -> empty");
    }

    // === 7) v1 BACK-COMPAT — a formatVersion-1 blob (no meta block) still unpacks; readMeta empty ===
    {
        auto v2 = packJson ([] { json j; j["architecture"] = "X"; j["weights"] = std::vector<double> { 0.5, 0.25, -0.5 }; return j; }(), true);
        // v1 = magic + ver(1) + codec + dtype + flags + body(from offset 10, dropping the u16 metaLen).
        bytes v1; v1.insert (v1.end(), v2.begin(), v2.begin() + 4);
        v1.push_back (1); v1.push_back (v2[5]); v1.push_back (v2[6]); v1.push_back (v2[7]);
        v1.insert (v1.end(), v2.begin() + 10, v2.end());
        auto back = unpackToJson (v1);
        CHECK (! back.is_null(), "v1: unpacks");
        CHECK (back["weights"].size() == 3, "v1: weights intact");
        CHECK (namz::readMeta (v1.data(), v1.size()).empty(), "v1: readMeta empty (no header block)");
    }

    // === 8) DEEP NESTING + MANY ARRAYS — recursion/scale must not crash or lose weights ===
    {
        json leaf; leaf["weights"] = std::vector<double> { 1.5, -2.5 };
        json deep = leaf;
        for (int i = 0; i < 90; ++i) { json w; w["child"] = deep; w["architecture"] = "N"; deep = w; }
        auto back = unpackToJson (packJson (deep, true));
        CHECK (! back.is_null(), "deep: 90-level nesting round-trips");

        json many; many["architecture"] = "X"; many["list"] = json::array();
        for (int i = 0; i < 800; ++i) many["list"].push_back ({ { "weights", std::vector<double> { (double) i, -(double) i } } });
        auto b2 = unpackToJson (packJson (many, true));
        CHECK (! b2.is_null() && b2["list"].size() == 800, "many: 800 weight arrays round-trip");
    }

    // === 9) UNICODE ZOO in metadata — scripts, emoji, RTL, escapes, controls survive verbatim ===
    {
        json j; j["architecture"] = "X"; j["weights"] = std::vector<double> { 1 };
        j["metadata"] = { { "cyr", "Привет мир" }, { "cjk", "日本語テスト" }, { "emoji", "🎸🔥🐈" },
                          { "rtl", "مرحبا" }, { "esc", "tab\tnl\nquote\"slash\\" }, { "mix", "café — ☃" } };
        auto back = unpackToJson (packJson (j, true));
        CHECK (! back.is_null() && back["metadata"] == j["metadata"], "unicode: full metadata zoo survives verbatim");
    }

    // === 10) OVERSIZED METADATA — a >64KB header is dropped gracefully; the model still round-trips ===
    {
        auto s = [] { json j; j["architecture"] = "X"; j["weights"] = std::vector<double> { 0.5, 0.5 }; return j.dump(); }();
        namz::PackOptions big;  big.metadata["huge"] = std::string (70000, 'a');   // > u16 header cap
        auto p = namz::pack (s.data(), s.size(), big);
        CHECK (! p.empty(), "bigmeta: still packs");
        CHECK (namz::readMeta (p.data(), p.size()).empty(), "bigmeta: >64KB header dropped (not overflowed)");
        auto back = unpackToJson (p);
        CHECK (! back.is_null() && back["metadata"]["huge"].get<std::string>().size() == 70000, "bigmeta: value still lives in the model (skeleton)");

        namz::PackOptions fit; fit.metadata["ok"] = std::string (60000, 'b');      // < u16 cap
        auto q = namz::pack (s.data(), s.size(), fit);
        CHECK (namz::readMeta (q.data(), q.size())["ok"].size() == 60000, "bigmeta: ~60KB header is readable");
    }

    // === 11) LOCALE — a comma-decimal locale must NOT corrupt the JSON number path (nlohmann is immune) ===
    {
        const char* got = std::setlocale (LC_ALL, "de_DE.UTF-8");
        if (got == nullptr) got = std::setlocale (LC_ALL, "de_DE");
        if (got != nullptr)
        {
            json j; j["architecture"] = "X"; j["weights"] = std::vector<double> { 0.1, 1234.5678, -0.0009 };
            auto back = unpackToJson (packJson (j, true));
            bool ok = ! back.is_null() && back["weights"].size() == 3;
            if (ok)
                for (int i = 0; i < 3; ++i)
                {
                    float a = (float) back["weights"][i].get<double>(), b = (float) j["weights"][i].get<double>();
                    std::uint32_t x, y; std::memcpy (&x, &a, 4); std::memcpy (&y, &b, 4); if (x != y) ok = false;
                }
            CHECK (ok, "locale: comma-decimal locale doesn't corrupt the wire (bit-exact)");
            std::setlocale (LC_ALL, "C");
        }
        else
        {
            std::fprintf (stderr, "note: de_DE locale unavailable, skipping locale test\n");
        }
    }

    // === 12) INTEGER weights — JSON integers must round-trip as the matching float32 ===
    {
        json j; j["architecture"] = "X"; j["weights"] = { 0, 1, -1, 16777216, -16777216 };   // exact in float32
        auto back = unpackToJson (packJson (j, true));
        bool ok = ! back.is_null();
        if (ok)
            for (std::size_t i = 0; i < j["weights"].size(); ++i)
            {
                float a = (float) back["weights"][i].get<double>(), b = (float) j["weights"][i].get<double>();
                std::uint32_t x, y; std::memcpy (&x, &a, 4); std::memcpy (&y, &b, 4); if (x != y) ok = false;
            }
        CHECK (ok, "int-weights: JSON integers round-trip as float32");
    }

    std::fprintf (stderr, "\n==== namz adversarial: %d checks, %d failures — %s ====\n",
                  checks, failures, failures == 0 ? "ALL PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
