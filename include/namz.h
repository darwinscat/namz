// SPDX-License-Identifier: MIT
// namz — a tiny lossless codec for NeuralAmpModeler `.nam` files.
// Single-header, dependency-light (C++17 std + nlohmann/json).
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.
//
// A `.nam` is JSON whose bulk is one or more flat `"weights"` arrays written as full-precision
// DECIMAL STRINGS (~20 chars/number). The NAM engine loads those weights into `std::vector<float>`
// (float32) — so the decimals are truncated to float32 on load ANYWAY. `.namz` stores each weight as a
// 4-byte float32 instead of ~20 bytes of text: ~5.5x smaller than the raw JSON and BIT-EXACT to what the
// engine computes (zero quality loss). Everything except the weight arrays (architecture/config/metadata)
// is preserved verbatim in a JSON skeleton.
//
// The float bytes are byte-plane SHUFFLED but NOT otherwise compressed (codec = store). The shuffle is
// free and helps whatever OUTER compressor sees the file (an installer's LZMA, git's packing); an inner
// deflate there is redundant, and storing raw keeps `.namz` DETERMINISTIC and byte-identical across
// platforms and runs, dependency-free, and fast to load.
//
// USAGE (single-header, stb-style): in exactly ONE translation unit, define the implementation:
//     #define NAMZ_IMPLEMENTATION
//     #include "namz.h"
// Everywhere else just `#include "namz.h"`. The implementation needs <nlohmann/json.hpp> on the include
// path. Endianness: integer fields are written little-endian explicitly; the float32 payload is stored in
// host byte order (every real NAM target — and NAM itself — is little-endian IEEE-754).
//
// Wire format (all multi-byte ints little-endian):
//   [0..3]  magic  'N','A','M','Z'
//   [4]     formatVersion (2; readers accept 1 = no meta block)
//   [5]     codec   (0 = store/uncompressed; 1 = deflate, 2 = zstd — reserved)
//   [6]     dtype   (0 = float32;             1 = float16 — reserved, lossy)
//   [7]     flags   (bit0 = weight bytes shuffled into 4 byte-planes)
//   [8..9]  metaLen (u16; v2 only) — bytes of a display-metadata JSON that follows, readable via
//           readMeta() without touching the weights. 0 = none.
//   [meta]  metaLen bytes of that JSON (v2 only)
//   [..]    body (codec 0 = stored verbatim):
//             u32 skeletonLen
//             u8  skeleton[skeletonLen]   (minified JSON; each numeric "weights" array replaced by its
//                                          ordinal index)
//             u32 numArrays
//             u32 lengths[numArrays]      (float count of each weights array, in index order)
//             u8  payload[]               (sum(lengths) * sizeof(dtype) bytes, byte-shuffled iff flags bit0)

#ifndef NAMZ_H
#define NAMZ_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace namz
{

inline constexpr std::uint8_t kFormatVersion = 2;

enum Codec : std::uint8_t { CodecStore = 0 /*, CodecDeflate = 1, CodecZstd = 2 (reserved) */ };
enum Dtype : std::uint8_t { DtypeF32   = 0 /*, DtypeF16 = 1 (reserved) */ };
enum Flags : std::uint8_t { FlagShuffle = 1u << 0 };

struct PackOptions
{
    bool shuffle = true;   // split float bytes into 4 planes (lossless) — free, and helps the OUTER
                           // compressor (installer LZMA / git packing) squeeze the stored `.namz`.

    // Optional string fields to set/overwrite in the top-level `metadata` object before packing, and
    // mirror into the readable header block. Typed on the way in: "true"/"false" -> bool, all-digits ->
    // integer, else string. Empty map = leave metadata untouched, no header block.
    std::map<std::string, std::string> metadata;
};

// True if `data` begins with the `.namz` magic. Cheap; safe on any/short buffer.
bool isNamz (const void* data, std::size_t n) noexcept;

// Read the display-metadata block (tone_type / device / gear_* / name / …) WITHOUT touching the weights.
// Empty for a v1 `.namz`, a non-`.namz` buffer, or one packed without metadata. Values are strings
// (bool -> "true"/"false", number -> its digits).
std::map<std::string, std::string> readMeta (const void* namz, std::size_t n);

// Parse NAM JSON (`.nam`) bytes -> packed `.namz`. Returns an EMPTY vector on failure (not valid JSON,
// non-numeric weights, etc.). Lossless w.r.t. the float32 engine.
std::vector<std::uint8_t> pack (const void* namJson, std::size_t n, const PackOptions& opts = {});

// Inverse: `.namz` bytes -> reconstructed `.nam` JSON bytes (weights rehydrated as float32 numbers).
// `maxJsonBytes` caps the reconstructed output (zip-bomb guard). Returns EMPTY on failure / over-cap /
// unknown codec|dtype / truncation / corruption.
std::vector<std::uint8_t> unpack (const void* namz, std::size_t n, std::size_t maxJsonBytes);

} // namespace namz

#ifdef NAMZ_IMPLEMENTATION

#include <algorithm>
#include <cstring>
#include <nlohmann/json.hpp>

namespace namz
{
namespace
{
    using json = nlohmann::json;
    constexpr char kMagic[4] = { 'N', 'A', 'M', 'Z' };

    inline void putU16 (std::vector<std::uint8_t>& o, std::uint16_t v)
    {
        o.push_back ((std::uint8_t) (v & 0xff));
        o.push_back ((std::uint8_t) ((v >> 8) & 0xff));
    }
    inline void putU32 (std::vector<std::uint8_t>& o, std::uint32_t v)
    {
        o.push_back ((std::uint8_t) (v & 0xff));
        o.push_back ((std::uint8_t) ((v >> 8) & 0xff));
        o.push_back ((std::uint8_t) ((v >> 16) & 0xff));
        o.push_back ((std::uint8_t) ((v >> 24) & 0xff));
    }
    inline std::uint16_t getU16 (const std::uint8_t* p)
    {
        return (std::uint16_t) ((std::uint16_t) p[0] | ((std::uint16_t) p[1] << 8));
    }
    inline std::uint32_t getU32 (const std::uint8_t* p)
    {
        return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8)
             | ((std::uint32_t) p[2] << 16) | ((std::uint32_t) p[3] << 24);
    }

    // byte-plane shuffle: AoS float bytes -> SoA planes (groups the structured sign/exponent bytes apart
    // from the noisy mantissa, so the OUTER compressor models them better). Reversible.
    void shuffleInto (const float* src, std::size_t count, std::uint8_t* dst)
    {
        const auto* s = reinterpret_cast<const std::uint8_t*> (src);
        for (std::size_t i = 0; i < count; ++i)
        {
            dst[0 * count + i] = s[4 * i + 0];
            dst[1 * count + i] = s[4 * i + 1];
            dst[2 * count + i] = s[4 * i + 2];
            dst[3 * count + i] = s[4 * i + 3];
        }
    }
    void unshuffleInto (const std::uint8_t* src, std::size_t count, float* dst)
    {
        auto* d = reinterpret_cast<std::uint8_t*> (dst);
        for (std::size_t i = 0; i < count; ++i)
        {
            d[4 * i + 0] = src[0 * count + i];
            d[4 * i + 1] = src[1 * count + i];
            d[4 * i + 2] = src[2 * count + i];
            d[4 * i + 3] = src[3 * count + i];
        }
    }

    bool isNumericWeights (const json& v)
    {
        if (! v.is_array())
            return false;
        for (const auto& x : v)
            if (! x.is_number())
                return false;
        return true;
    }

    // Type a metadata string for JSON: "true"/"false" -> bool, all-digits -> integer, else string.
    json typeValue (const std::string& s)
    {
        if (s == "true")  return true;
        if (s == "false") return false;
        if (! s.empty() && s.find_first_not_of ("0123456789") == std::string::npos)
        {
            try { return (std::int64_t) std::stoll (s); }
            catch (...) { return s; }   // too big for int64 -> keep as string
        }
        return s;
    }

    // DFS: pull every numeric "weights" array out into `out` (in traversal order) and replace its value in
    // the tree with its ordinal index (a JSON integer). The stripped tree is the skeleton.
    void extractWeights (json& node, std::vector<std::vector<float>>& out)
    {
        if (node.is_object())
        {
            for (auto it = node.begin(); it != node.end(); ++it)
            {
                if (it.key() == "weights" && isNumericWeights (it.value()))
                {
                    std::vector<float> w;
                    w.reserve (it.value().size());
                    for (const auto& x : it.value())
                        w.push_back ((float) x.get<double>());
                    out.push_back (std::move (w));
                    it.value() = (std::int64_t) (out.size() - 1);
                }
                else
                {
                    extractWeights (it.value(), out);
                }
            }
        }
        else if (node.is_array())
        {
            for (auto& v : node)
                extractWeights (v, out);
        }
    }

    // DFS inverse: wherever a "weights" key holds an integer index, swap in that float segment. `ok` is
    // cleared if any placeholder can't be filled (idx out of range) — the signature of a corrupt stream.
    void refillWeights (json& node, const std::vector<std::vector<float>>& segs, bool& ok)
    {
        if (node.is_object())
        {
            for (auto it = node.begin(); it != node.end(); ++it)
            {
                if (it.key() == "weights" && it.value().is_number_integer())
                {
                    const auto idx = it.value().get<long long>();
                    if (idx >= 0 && idx < (long long) segs.size())
                        it.value() = segs[(std::size_t) idx];
                    else
                        ok = false;
                }
                else
                {
                    refillWeights (it.value(), segs, ok);
                }
            }
        }
        else if (node.is_array())
        {
            for (auto& v : node)
                refillWeights (v, segs, ok);
        }
    }
} // namespace

bool isNamz (const void* data, std::size_t n) noexcept
{
    return data != nullptr && n >= sizeof (kMagic) && std::memcmp (data, kMagic, sizeof (kMagic)) == 0;
}

std::vector<std::uint8_t> pack (const void* namJson, std::size_t n, const PackOptions& opts)
{
    static_assert (sizeof (float) == 4, "namz assumes 32-bit IEEE-754 float");

    std::vector<std::vector<float>> arrays;
    std::string skeleton;
    std::string headerMeta;
    try
    {
        auto j = json::parse (static_cast<const char*> (namJson),
                              static_cast<const char*> (namJson) + n);

        if (! opts.metadata.empty())
        {
            if (! j.contains ("metadata") || ! j["metadata"].is_object())
                j["metadata"] = json::object();
            json hdr = json::object();
            for (const auto& kv : opts.metadata)
            {
                const auto val = typeValue (kv.second);
                j["metadata"][kv.first] = val;
                hdr[kv.first]           = val;
            }
            headerMeta = hdr.dump();
            if (headerMeta.size() > 0xFFFF)   // u16 length field — display metadata is tiny; never hit
                headerMeta.clear();
        }

        extractWeights (j, arrays);
        skeleton = j.dump();
    }
    catch (...) { return {}; }

    std::size_t totalFloats = 0;
    for (const auto& a : arrays)
        totalFloats += a.size();

    std::vector<std::uint8_t> body;
    putU32 (body, (std::uint32_t) skeleton.size());
    body.insert (body.end(), skeleton.begin(), skeleton.end());
    putU32 (body, (std::uint32_t) arrays.size());
    for (const auto& a : arrays)
        putU32 (body, (std::uint32_t) a.size());

    if (opts.shuffle && totalFloats > 0)
    {
        std::vector<float> flat;
        flat.reserve (totalFloats);
        for (const auto& a : arrays)
            flat.insert (flat.end(), a.begin(), a.end());
        std::vector<std::uint8_t> shuffled (totalFloats * 4);
        shuffleInto (flat.data(), totalFloats, shuffled.data());
        body.insert (body.end(), shuffled.begin(), shuffled.end());
    }
    else
    {
        for (const auto& a : arrays)
            if (! a.empty())
            {
                const auto* p = reinterpret_cast<const std::uint8_t*> (a.data());
                body.insert (body.end(), p, p + a.size() * sizeof (float));
            }
    }

    std::vector<std::uint8_t> out;
    out.insert (out.end(), kMagic, kMagic + sizeof (kMagic));
    out.push_back (kFormatVersion);
    out.push_back (CodecStore);
    out.push_back (DtypeF32);
    out.push_back ((std::uint8_t) (opts.shuffle ? FlagShuffle : 0));
    putU16 (out, (std::uint16_t) headerMeta.size());
    out.insert (out.end(), headerMeta.begin(), headerMeta.end());
    out.insert (out.end(), body.begin(), body.end());
    return out;
}

std::vector<std::uint8_t> unpack (const void* namz, std::size_t n, std::size_t maxJsonBytes)
{
    if (! isNamz (namz, n) || n < 8)
        return {};

    const auto* bytes = static_cast<const std::uint8_t*> (namz);
    const std::uint8_t fmt   = bytes[4];
    const std::uint8_t codec = bytes[5];
    const std::uint8_t dtype = bytes[6];
    const std::uint8_t flags = bytes[7];
    if (fmt > kFormatVersion || codec != CodecStore || dtype != DtypeF32)
        return {};

    std::size_t off = 8;
    if (fmt >= 2)
    {
        if (n < 10) return {};
        off = 10 + (std::size_t) getU16 (bytes + 8);
        if (off > n) return {};
    }

    // Body is stored verbatim. Cap the raw size against the reconstructed-JSON cap so a crafted oversized
    // blob can't force a huge allocation before the per-field checks below catch it.
    if (n - off > maxJsonBytes + 4096)
        return {};

    const std::uint8_t* p = bytes + off;
    std::size_t rem = n - off;   // bytes still unread from `p`

    try
    {
        if (rem < 4) return {};
        const std::size_t skeletonLen = getU32 (p); p += 4; rem -= 4;
        if (skeletonLen > rem) return {};
        std::string skeleton (reinterpret_cast<const char*> (p), skeletonLen);
        p += skeletonLen; rem -= skeletonLen;

        if (rem < 4) return {};
        const std::size_t numArrays = getU32 (p); p += 4; rem -= 4;
        if (numArrays > rem / 4) return {};   // can't hold that many u32 lengths -> truncated / OOM guard
        std::vector<std::size_t> lengths (numArrays);
        std::size_t totalFloats = 0;
        for (std::size_t i = 0; i < numArrays; ++i)
        {
            lengths[i] = getU32 (p); p += 4; rem -= 4;
            totalFloats += lengths[i];
        }

        // The float32 payload must be EXACTLY the rest of the buffer (division, not multiply -> no
        // overflow). Rejects truncation and lying/short lengths that would slice garbage into the weights.
        if (rem % sizeof (float) != 0 || totalFloats != rem / sizeof (float))
            return {};

        std::vector<std::vector<float>> segs (numArrays);
        if (totalFloats > 0)
        {
            std::vector<float> flat (totalFloats);
            if ((flags & FlagShuffle) != 0)
                unshuffleInto (p, totalFloats, flat.data());
            else
                std::memcpy (flat.data(), p, totalFloats * sizeof (float));

            std::size_t o = 0;
            for (std::size_t i = 0; i < numArrays; ++i)
            {
                segs[i].assign (flat.begin() + (std::ptrdiff_t) o,
                                flat.begin() + (std::ptrdiff_t) (o + lengths[i]));
                o += lengths[i];
            }
        }

        auto j = json::parse (skeleton);
        bool refillOk = true;
        refillWeights (j, segs, refillOk);
        if (! refillOk) return {};
        const auto rebuilt = j.dump();
        if (rebuilt.size() > maxJsonBytes)
            return {};

        return std::vector<std::uint8_t> (rebuilt.begin(), rebuilt.end());
    }
    catch (...) { return {}; }
}

std::map<std::string, std::string> readMeta (const void* namz, std::size_t n)
{
    std::map<std::string, std::string> out;
    if (! isNamz (namz, n) || n < 10)
        return out;
    const auto* bytes = static_cast<const std::uint8_t*> (namz);
    if (bytes[4] < 2)                    // v1 has no meta block
        return out;
    const std::size_t metaLen = (std::size_t) getU16 (bytes + 8);
    if (metaLen == 0 || 10 + metaLen > n)
        return out;
    try
    {
        auto j = json::parse (bytes + 10, bytes + 10 + metaLen);
        if (j.is_object())
            for (auto it = j.begin(); it != j.end(); ++it)
            {
                const auto& v = it.value();
                if      (v.is_string())         out[it.key()] = v.get<std::string>();
                else if (v.is_boolean())        out[it.key()] = v.get<bool>() ? "true" : "false";
                else if (v.is_number_integer()) out[it.key()] = std::to_string (v.get<long long>());
                else if (v.is_number())         out[it.key()] = std::to_string (v.get<double>());
                else                            out[it.key()] = v.dump();
            }
    }
    catch (...) {}
    return out;
}

} // namespace namz

#endif // NAMZ_IMPLEMENTATION
#endif // NAMZ_H
