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
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <vector>

using json = nlohmann::json;
using bytes = std::vector<std::uint8_t>;

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (! (cond)) { ++failures; std::fprintf (stderr, "FAIL: %s\n", msg); } } while (0)

// Every byte the program asks of operator new while `gCounting` is set — how a test sees WHEN a reader
// allocates, not just whether it refuses.
static std::size_t gAllocated = 0;
static bool gCounting = false;
void* operator new (std::size_t n)
{
    if (gCounting) gAllocated += n;
    if (void* p = std::malloc (n != 0 ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n) { return operator new (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

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

static constexpr std::size_t kMaxFloats = 64u * 1024u * 1024u;

// namz::decode must agree with unpack on ANY bytes: refuse exactly what unpack refuses, and where both
// accept, its skeleton refilled and dumped must be unpack's output byte for byte.
static bool decodeAgrees (const std::uint8_t* data, std::size_t len)
{
    const auto u = namz::unpack (data, len, kCap);
    const auto d = namz::decode (data, len, kMaxFloats);
    if (u.empty() != ! d.has_value()) return false;
    if (! d) return true;
    json full = d->skeleton;
    if (! d->refill (full)) return false;
    const auto s = full.dump();
    return bytes (s.begin(), s.end()) == u;
}
static bool decodeAgrees (const bytes& b) { return decodeAgrees (b.data(), b.size()); }

// A v2 container written by hand: any skeleton, any lengths, `payloadFloats` floats of payload — the
// lengths need not sum to it, which is the point.
static bytes craft (const std::string& skeleton, const std::vector<std::uint32_t>& lengths, std::size_t payloadFloats)
{
    auto u32 = [] (bytes& o, std::uint32_t v) { for (int k = 0; k < 4; ++k) o.push_back ((std::uint8_t) (v >> (8 * k))); };
    bytes o { 'N', 'A', 'M', 'Z', 2, 0, 0, 0, 0, 0 };
    u32 (o, (std::uint32_t) skeleton.size());
    o.insert (o.end(), skeleton.begin(), skeleton.end());
    u32 (o, (std::uint32_t) lengths.size());
    for (auto l : lengths) u32 (o, l);
    for (std::size_t i = 0; i < payloadFloats; ++i) { const float f = 0.5f + (float) i; const auto* fb = reinterpret_cast<const std::uint8_t*> (&f); o.insert (o.end(), fb, fb + 4); }
    return o;
}

// `depth` arrays around a 0: nesting depth `depth` by the format's rule — a scalar is 0, an array or object is
// 1 + the deepest of its members.
static std::string nested (std::size_t depth) { return std::string (depth, '[') + "0" + std::string (depth, ']'); }

// `blob` (from craft, metaLen 0) with a display-metadata block of text `meta` in front of its body.
static bytes withMeta (const bytes& blob, const std::string& meta)
{
    bytes o (blob.begin(), blob.begin() + 8);
    o.push_back ((std::uint8_t) (meta.size() & 0xff));
    o.push_back ((std::uint8_t) ((meta.size() >> 8) & 0xff));
    o.insert (o.end(), meta.begin(), meta.end());
    o.insert (o.end(), blob.begin() + 10, blob.end());
    return o;
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

    // === 13) DECODE vs UNPACK — the second reader refuses what the first refuses, everywhere ===
    {
        // a container, both payload layouts, and its v1 spelling: truncated at EVERY offset, +1 byte
        json inner; inner["architecture"] = "WaveNet"; inner["config"] = { { "layers", 3 } };
        inner["weights"] = std::vector<double> { 0.5, -0.25, 3.0e-39, -0.0 };
        json c; c["architecture"] = "SlimmableContainer"; c["version"] = "0.7.0";
        c["config"]["submodels"] = json::array ({ { { "max_value", 1.0 }, { "model", inner } } });
        c["weights"] = std::vector<double> { 1.5, 2.5, -3.5 };
        for (bool shuffle : { true, false })
        {
            const auto good = packJson (c, shuffle);
            bytes v1 (good.begin(), good.begin() + 4);
            v1.push_back (1); v1.push_back (good[5]); v1.push_back (good[6]); v1.push_back (good[7]);
            v1.insert (v1.end(), good.begin() + 10, good.end());
            for (const bytes* blob : { &good, (const bytes*) &v1 })
            {
                bool agrees = true, refusesShort = true;
                for (std::size_t len = 0; len <= blob->size(); ++len)
                {
                    if (! decodeAgrees (blob->data(), len)) agrees = false;
                    if (len < blob->size() && namz::decode (blob->data(), len, kMaxFloats).has_value()) refusesShort = false;
                }
                auto plus = *blob; plus.push_back (0);
                CHECK (agrees && refusesShort && decodeAgrees (*blob) && namz::decode (blob->data(), blob->size(), kMaxFloats).has_value(),
                       "decode: truncated at every offset -> refused, as unpack refuses it; the whole blob decodes");
                CHECK (! namz::decode (plus.data(), plus.size(), kMaxFloats).has_value() && decodeAgrees (plus), "decode: +1 byte refused");
            }
        }

        // bit-flip fuzz: whatever corruption unpack survives or refuses, decode lands on the same verdict
        {
            const auto good = packJson (c, true);
            std::mt19937_64 rng (0x5EED5u);
            int disagreements = 0;
            for (int iter = 0; iter < 8000; ++iter)
            {
                auto m = good;
                const int flips = 1 + (int) (rng() % 3);
                for (int f = 0; f < flips; ++f)
                    m[(std::size_t) (rng() % m.size())] ^= (std::uint8_t) (1u << (rng() % 8));
                if (! decodeAgrees (m)) ++disagreements;
            }
            CHECK (disagreements == 0, "decode: 8000 corruptions -> the verdict unpack reaches, and unpack's bytes when accepted");
        }

        // hand-crafted: lying skeletonLen / numArrays / metaLen, every version/codec/dtype byte
        {
            const auto base = packJson (c, true);
            const std::uint32_t skLen = rdU32 (base, 10);
            bool agrees = true;
            for (std::size_t off : { (std::size_t) 10, (std::size_t) (14 + skLen), (std::size_t) (18 + skLen) })
                for (std::uint32_t v : { 0u, 1u, 99u, 0x7FFFFFFFu, 0xFFFFFFFFu })
                {
                    auto m = base;
                    for (int k = 0; k < 4; ++k) m[off + (std::size_t) k] = (std::uint8_t) (v >> (8 * k));
                    if (! decodeAgrees (m)) agrees = false;
                }
            { auto m = base; m[8] = 0xff; m[9] = 0xff; if (! decodeAgrees (m)) agrees = false; }
            for (std::size_t at : { 4, 5, 6, 7 })
                for (int v = 0; v < 256; ++v) { auto m = base; m[at] = (std::uint8_t) v; if (! decodeAgrees (m)) agrees = false; }
            CHECK (agrees, "decode: lying lengths / metaLen / every header byte -> unpack's verdict");
            CHECK (! namz::decode (nullptr, 0, kMaxFloats).has_value() && ! namz::decode (base.data(), 4, kMaxFloats).has_value(),
                   "decode: null buffer and magic-only refused");
        }

        // lengths that overflow: each refused, by both readers — a 32-bit size_t would wrap these sums
        {
            const std::vector<std::vector<std::uint32_t>> lying {
                { 2u, 0xFFFFFFFFu }, { 0xFFFFFFFFu, 2u }, { 0xFFFFFFFFu, 0xFFFFFFFFu, 3u },
                { 0x10000000u, 0xF0000001u }, { 0x80000000u, 0x80000000u, 1u } };
            bool refused = true;
            for (const auto& lens : lying)
            {
                const auto m = craft ("{\"weights\":0}", lens, 1);
                if (namz::decode (m.data(), m.size(), kMaxFloats).has_value() || ! namz::unpack (m.data(), m.size(), kCap).empty())
                    refused = false;
            }
            CHECK (refused, "decode: lengths whose sum overflows or wraps -> refused by decode and unpack");
        }

        // a placeholder that names no segment: refused by both, whatever its integer
        {
            bool refused = true;
            for (const char* sk : { "{\"weights\":1}", "{\"weights\":-1}", "{\"weights\":18446744073709551615}",
                                    "{\"a\":[{\"weights\":0},{\"weights\":5}]}" })
            {
                const auto m = craft (sk, { 2u }, 2);
                if (namz::decode (m.data(), m.size(), kMaxFloats).has_value() || ! namz::unpack (m.data(), m.size(), kCap).empty())
                    refused = false;
            }
            const auto bad = craft ("{\"weights\":", { 1u }, 1);   // the skeleton is not JSON
            CHECK (refused && ! namz::decode (bad.data(), bad.size(), kMaxFloats).has_value() && namz::unpack (bad.data(), bad.size(), kCap).empty(),
                   "decode: a placeholder naming no segment, or a skeleton that is not JSON -> refused by both");
        }

        // the zip bomb: one segment named K times. unpack's JSON cap and decode's float cap both stop it,
        // decode at exactly K * len named floats; the stored floats alone are capped too.
        {
            const std::size_t K = 1000, len = 1000;
            std::string sk = "[";
            for (std::size_t i = 0; i < K; ++i) sk += (i ? ",{\"weights\":0}" : "{\"weights\":0}");
            sk += "]";
            const auto bomb = craft (sk, { (std::uint32_t) len }, len);
            const auto ok = namz::decode (bomb.data(), bomb.size(), K * len);
            CHECK (ok.has_value() && ok->segments.size() == 1 && ok->segments[0].size() == len,
                   "decode: K placeholders on one segment decode at maxFloats == K * len, holding the segment once");
            CHECK (! namz::decode (bomb.data(), bomb.size(), K * len - 1).has_value(), "decode: one float short of K * len -> refused");
            CHECK (namz::unpack (bomb.data(), bomb.size(), bomb.size() * 10).empty(), "decode: (unpack refuses the same bomb by its JSON cap)");
            const auto flat = craft ("{\"weights\":0}", { (std::uint32_t) len }, len);
            CHECK (namz::decode (flat.data(), flat.size(), len).has_value() && ! namz::decode (flat.data(), flat.size(), len - 1).has_value(),
                   "decode: the stored floats are capped as well");
            const auto unnamed = craft ("{\"a\":1}", { (std::uint32_t) len }, len);   // a segment nothing names
            CHECK (decodeAgrees (unnamed) && ! namz::decode (unnamed.data(), unnamed.size(), len - 1).has_value(),
                   "decode: a segment no placeholder names still counts against the cap, and is accepted as unpack accepts it");
        }

        // deep nesting and many arrays decode as they unpack
        {
            json leaf; leaf["weights"] = std::vector<double> { 1.5, -2.5 };
            json deep = leaf;
            for (int i = 0; i < 90; ++i) { json w; w["child"] = deep; w["architecture"] = "N"; deep = w; }
            json many; many["architecture"] = "X"; many["list"] = json::array();
            for (int i = 0; i < 800; ++i) many["list"].push_back ({ { "weights", std::vector<double> { (double) i, -(double) i } } });
            const auto pm = packJson (many, true);
            const auto dm = namz::decode (pm.data(), pm.size(), kMaxFloats);
            CHECK (decodeAgrees (packJson (deep, true)) && decodeAgrees (pm) && dm.has_value() && dm->segments.size() == 800
                   && dm->weightsOf (dm->skeleton["list"][799]) != nullptr && (*dm->weightsOf (dm->skeleton["list"][799]))[0] == 799.0f,
                   "decode: 90-level nesting and 800 arrays decode as they unpack");
        }
    }

    // === 14) THE STATED RULES — placeholder edges, the depth limit, the cap before any allocation ===
    {
        // a placeholder at its edges: an integer literal that fits int64 or uint64 names a segment, -0 included;
        // a fraction, an exponent, or an integer past uint64 is a plain number and stays one; an integer that
        // names no segment is refused by both readers
        {
            struct Edge { const char* skeleton; const char* unpacked; };   // unpacked == nullptr: refused
            const Edge edges[] {
                { "{\"weights\":-0}",                   "{\"weights\":[0.5,1.5]}" },
                { "{\"weights\":1.0}",                  "{\"weights\":1.0}" },
                { "{\"weights\":0e0}",                  "{\"weights\":0.0}" },
                { "{\"weights\":18446744073709551616}", "{\"weights\":1.8446744073709552e+19}" },
                { "{\"weights\":9223372036854775808}",  nullptr } };
            for (const auto& e : edges)
            {
                const auto m = craft (e.skeleton, { 2u }, 2);
                const auto u = namz::unpack (m.data(), m.size(), kCap);
                const bool ok = decodeAgrees (m)
                             && (e.unpacked == nullptr ? u.empty() : std::string (u.begin(), u.end()) == e.unpacked);
                const std::string msg = std::string ("placeholder edge: ") + e.skeleton;
                CHECK (ok, msg.c_str());
            }
        }

        // depth 128 is read; 129 is refused by both readers, the skeleton's depth counted as the format states
        const auto deepSkeleton = [] (std::size_t depth) { return "{\"weights\":0,\"x\":" + nested (depth - 1) + "}"; };
        const auto deepNam      = [] (std::size_t depth) { return "{\"weights\":[1.5],\"x\":" + nested (depth - 1) + "}"; };
        {
            const auto at128 = craft (deepSkeleton (128), { 2u }, 2), bare128 = craft (nested (128), {}, 0);
            const auto at129 = craft (deepSkeleton (129), { 2u }, 2), bare129 = craft (nested (129), {}, 0);
            CHECK (decodeAgrees (at128) && namz::decode (at128.data(), at128.size(), kMaxFloats).has_value()
                   && decodeAgrees (bare128) && ! namz::unpack (bare128.data(), bare128.size(), kCap).empty(),
                   "depth: a skeleton 128 deep is read by unpack and by decode");
            CHECK (namz::unpack (at129.data(), at129.size(), kCap).empty() && ! namz::decode (at129.data(), at129.size(), kMaxFloats).has_value()
                   && namz::unpack (bare129.data(), bare129.size(), kCap).empty() && ! namz::decode (bare129.data(), bare129.size(), kMaxFloats).has_value(),
                   "depth: a skeleton 129 deep is refused by unpack and by decode");
        }
        // pack holds a .nam to the same depth, so everything it writes reads back
        {
            const auto s128 = deepNam (128), s129 = deepNam (129);
            const auto p128 = namz::pack (s128.data(), s128.size());
            CHECK (! p128.empty() && unpackToJson (p128) == json::parse (s128) && decodeAgrees (p128),
                   "depth: pack takes a .nam 128 deep, and it reads back");
            CHECK (namz::pack (s129.data(), s129.size()).empty(), "depth: pack refuses a .nam 129 deep");
        }
        // readMeta: a meta block deeper than 128 reads as no metadata
        {
            const auto base = craft ("{\"weights\":0}", { 1u }, 1);
            const auto m128 = withMeta (base, "{\"a\":" + nested (127) + "}");
            const auto m129 = withMeta (base, "{\"a\":" + nested (128) + "}");
            CHECK (namz::readMeta (m128.data(), m128.size()).size() == 1, "depth: readMeta reads a meta block 128 deep");
            CHECK (namz::readMeta (m129.data(), m129.size()).empty(), "depth: readMeta returns nothing for a meta block 129 deep");
        }

        // the stored-floats cap comes before any allocation: a million lengths and one float of payload,
        // over a cap of zero, cost not one byte
        {
            std::vector<std::uint32_t> lens (1000000, 0u);
            lens[0] = 1;
            const auto m = craft ("{\"weights\":0}", lens, 1);
            gAllocated = 0;
            gCounting = true;
            const bool refused = ! namz::decode (m.data(), m.size(), 0).has_value();
            gCounting = false;
            if (gAllocated != 0) std::fprintf (stderr, "      (allocated %zu bytes before refusing)\n", gAllocated);
            CHECK (refused && gAllocated == 0, "decode: over the stored-floats cap -> refused before a single byte is allocated");
        }

        // last, because this is where the suite used to die: nesting far past the limit is refused without
        // following it, so the stack never sees it
        {
            const auto blob = craft (deepSkeleton (100000), { 2u }, 2);
            CHECK (namz::unpack (blob.data(), blob.size(), kCap).empty() && ! namz::decode (blob.data(), blob.size(), kMaxFloats).has_value(),
                   "depth: a skeleton 100,000 deep is refused by unpack and by decode, the stack intact");
            const auto nam = deepNam (100000);
            CHECK (namz::pack (nam.data(), nam.size()).empty(), "depth: pack refuses a .nam 100,000 deep, the stack intact");
            const auto meta = withMeta (craft ("{\"weights\":0}", { 1u }, 1), "{\"a\":" + nested (32000) + "}");   // near the u16 block's most
            CHECK (namz::readMeta (meta.data(), meta.size()).empty(), "depth: readMeta returns nothing for a meta block 32,001 deep, the stack intact");
        }
    }

    std::fprintf (stderr, "\n==== namz adversarial: %d checks, %d failures — %s ====\n",
                  checks, failures, failures == 0 ? "ALL PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
