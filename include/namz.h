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
// Two ways to read one back. `unpack` rebuilds the `.nam` JSON text, for anything that takes a `.nam`.
// `decode` hands over what the file holds — the skeleton already parsed and the weights as float32
// vectors — for a loader that builds its model itself: the weights never become decimal text, so they are
// never printed and never parsed back.
//
// USAGE (single-header, stb-style): in exactly ONE translation unit, define the implementation:
//     #define NAMZ_IMPLEMENTATION
//     #include "namz.h"
// Everywhere else just `#include "namz.h"`. The header needs <nlohmann/json.hpp> on the include path
// (`decode` hands out a parsed skeleton), and every translation unit that includes it must see the SAME
// nlohmann/json, configured the same way (JSON_DIAGNOSTICS and the rest), as the one that defines
// NAMZ_IMPLEMENTATION: `decode`'s mangled name carries no nlohmann type, so a mismatch is no link error —
// it links silently against a `Decoded` of another layout. Endianness: integer fields are written little-endian
// explicitly; the float32 payload is stored in host byte order (every real NAM target — and NAM itself —
// is little-endian IEEE-754).
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

#define NAMZ_VERSION_MAJOR 4
#define NAMZ_VERSION_MINOR 3
#define NAMZ_VERSION_PATCH 1
#define NAMZ_VERSION "4.3.1"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace namz
{

inline constexpr std::uint8_t kFormatVersion = 2;   // what pack writes; readers take 1 and 2, nothing else

// The deepest any JSON in a `.namz` may nest. Depth: a scalar is 0; an array or object is 1 + the deepest of
// its members, so an empty one is 1. `unpack` and `decode` refuse a skeleton deeper than this, `pack` refuses
// a `.nam` deeper than this (so everything it writes reads back), and `readMeta` reads a deeper meta block as
// no metadata. Real captures nest about 10 deep. The limit is what keeps every recursive walk over the tree —
// namz's own, nlohmann's dump and compare, a loader's model builder — off the end of the stack.
inline constexpr std::size_t kMaxDepth = 128;

enum Codec : std::uint8_t { CodecStore = 0 /*, CodecDeflate = 1, CodecZstd = 2 (reserved) */ };
enum Dtype : std::uint8_t { DtypeF32   = 0 /*, DtypeF16 = 1 (reserved) */ };
enum Flags : std::uint8_t { FlagShuffle = 1u << 0 };

struct PackOptions
{
    bool shuffle = true;   // split float bytes into 4 planes (lossless) — free, and helps the OUTER
                           // compressor (installer LZMA / git packing) squeeze the stored `.namz`.

    // Optional string fields to set/overwrite in the top-level `metadata` object before packing, and
    // mirror into the readable header block. Typed on the way in: "true"/"false" -> bool, all-digits ->
    // integer, else string — except that a LEADING ZERO keeps the value a string, because "0012345" is
    // an identifier and its zeros are part of it. Empty map = leave metadata untouched, no header block.
    std::map<std::string, std::string> metadata;
};

// True if `data` begins with the `.namz` magic. Cheap; safe on any/short buffer.
bool isNamz (const void* data, std::size_t n) noexcept;

// Read the display-metadata block (tone_type / device / gear_* / name / …) WITHOUT touching the weights.
// Empty for a v1 `.namz`, a non-`.namz` buffer, or one packed without metadata. Values are strings
// (bool -> "true"/"false", integer -> its digits, signed or unsigned as written, other number -> "%f").
std::map<std::string, std::string> readMeta (const void* namz, std::size_t n);

// Parse NAM JSON (`.nam`) bytes -> packed `.namz`. Returns an EMPTY vector on failure (not valid JSON,
// nested deeper than kMaxDepth, a "weights" member holding an integer literal — the form a placeholder
// takes, so it could not read back as itself — a weight whose float32 is not finite, etc.). Lossless w.r.t.
// the float32 engine.
std::vector<std::uint8_t> pack (const void* namJson, std::size_t n, const PackOptions& opts = {});

// Inverse: `.namz` bytes -> reconstructed `.nam` JSON bytes (weights rehydrated as float32 numbers).
// `maxJsonBytes` caps the reconstructed output (zip-bomb guard). Returns EMPTY on failure / over-cap /
// unknown version|codec|dtype / truncation / corruption / a payload float that is NaN or infinite.
std::vector<std::uint8_t> unpack (const void* namz, std::size_t n, std::size_t maxJsonBytes);

// What a `.namz` holds, read without making a single weight a JSON number. Owns everything; nothing in it
// points back into the bytes it was decoded from.
struct Decoded
{
    // The stored skeleton, parsed: the `.nam` with every numeric "weights" array replaced by the integer
    // index of its segment, exactly as the file stores it (keys sorted), no deeper than kMaxDepth. version / architecture / config /
    // metadata / sample_rate are here as the `.nam` had them; a SlimmableContainer's submodels sit at
    // config.submodels[i].model, each with its own "weights" placeholder.
    nlohmann::json skeleton;

    // The float32 weights, one vector per segment, in index order — the very floats `unpack` prints.
    // A segment is a std::vector<float> because that is what NAM's engine takes: it moves or copies
    // straight in, with no reshaping on the way.
    std::vector<std::vector<float>> segments;

    // The segment `node`'s "weights" placeholder names — `weightsOf (skeleton)` is the top-level model's,
    // `weightsOf (skeleton["config"]["submodels"][i]["model"])` a submodel's. nullptr when `node` holds
    // no placeholder: not an object, no "weights" key, or a "weights" that is not an integer. Within this
    // skeleton a placeholder always names a segment: decode refused any file where one did not.
    const std::vector<float>* weightsOf (const nlohmann::json& node) const;

    // Swap every placeholder under `node`, in place, for its segment as JSON numbers — the tree `unpack`
    // dumps, so `auto j = d.skeleton; d.refill (j); j.dump()` is byte for byte unpack's output. For a
    // loader that needs one subtree (a config holding a nested capture) as `.nam` JSON. False when a
    // placeholder names no segment (a node from elsewhere); `node` is then partly filled — discard it.
    bool refill (nlohmann::json& node) const;
};

// `.namz` bytes -> its skeleton and its float32 segments, the weights never printed or parsed. nullopt on
// exactly what `unpack` refuses — bad magic, unknown version|codec|dtype, truncation, trailing bytes,
// lengths that do not sum to the payload, a skeleton that is not JSON or nests deeper than kMaxDepth, a
// placeholder naming no segment, a payload float that is NaN or infinite — with the size guard counted in floats rather than JSON bytes, because no
// JSON is ever produced. `maxFloats` caps two things:
//   - the floats the file stores, i.e. what decode allocates for them — checked before anything at all is
//     allocated;
//   - the floats its placeholders name, a segment named twice counted twice: what a loader filling every
//     placeholder holds. Duplicate placeholders are the one way a small file asks for a large model —
//     the zip bomb `unpack`'s maxJsonBytes stops by the size of the text it would print. Checked once the
//     skeleton is parsed (straight from the buffer, so bounded by `n`), before a single float is allocated.
// The cap counts floats, not what a caller makes of them: `refill` turns each 4-byte float into a JSON
// number node, several times its size.
std::optional<Decoded> decode (const void* namz, std::size_t n, std::size_t maxFloats);

} // namespace namz

#ifdef NAMZ_IMPLEMENTATION

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
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

    // A float32 that is a number: its exponent bits are not all ones (that is NaN or an infinity). Read off
    // the bits, so no floating-point mode or flag can change the answer.
    bool finiteF32 (float f)
    {
        std::uint32_t bits;
        std::memcpy (&bits, &f, sizeof bits);
        return (bits & 0x7F800000u) != 0x7F800000u;
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
    //
    // A LEADING ZERO IS NOT A DIGIT OF A QUANTITY. "0012345" is somebody's identifier — a serial
    // number read off the back of a pedal, a catalogue code — and the zeros in front of it are part
    // of what is stamped there. Typed to 12345 it comes back as a different string than the one that
    // went in, and the same box then reads one way out of the manifest (where it is a string) and
    // another out of its own file header. JSON says the same thing about its own literals — a number
    // may not start with a redundant zero — so this keeps the typed value and the text agreeing.
    // Bare "0" is the quantity zero and stays one.
    //
    // Decided by the VALUE, never by the key: this codec does not know which format is stamping it,
    // and a list of field names in here would be one more thing to keep in sync with a header it has
    // never heard of.
    json typeValue (const std::string& s)
    {
        if (s == "true")  return true;
        if (s == "false") return false;
        if (! s.empty() && s.find_first_not_of ("0123456789") == std::string::npos
            && (s.size() == 1 || s[0] != '0'))
        {
            try { return (std::int64_t) std::stoll (s); }
            catch (...) { return s; }   // too big for int64 -> keep as string
        }
        return s;
    }

    // DFS: pull every numeric "weights" array out into `out` (in traversal order) and replace its value in
    // the tree with its ordinal index (a JSON integer). The stripped tree is the skeleton. False if a
    // "weights" member already holds an integer: the skeleton would store it as a placeholder, and a reader
    // would swap a segment in for it (config.weights 0 came back as the top-level array) or refuse the file
    // ("weights":7 named no segment). A placeholder is pack's to write, never the input's. False too if a
    // weight's float32 is not finite: 1e39 is a finite JSON number past the float32 range, and the payload
    // never stores NaN or an infinity (the readers refuse one).
    bool extractWeights (json& node, std::vector<std::vector<float>>& out)
    {
        if (node.is_object())
        {
            for (auto it = node.begin(); it != node.end(); ++it)
            {
                if (it.key() == "weights" && it.value().is_number_integer())
                    return false;
                if (it.key() == "weights" && isNumericWeights (it.value()))
                {
                    std::vector<float> w;
                    w.reserve (it.value().size());
                    for (const auto& x : it.value())
                    {
                        w.push_back ((float) x.get<double>());
                        if (! finiteF32 (w.back()))
                            return false;
                    }
                    out.push_back (std::move (w));
                    it.value() = (std::int64_t) (out.size() - 1);
                }
                else if (! extractWeights (it.value(), out))
                {
                    return false;
                }
            }
        }
        else if (node.is_array())
        {
            for (auto& v : node)
                if (! extractWeights (v, out))
                    return false;
        }
        return true;
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

    // unshuffleInto for one segment: `count` floats starting at float `first` of a payload of `total`
    // floats, straight into the segment's own storage (plane b of float k sits at b * total + k).
    void unshuffleSegment (const std::uint8_t* planes, std::size_t total, std::size_t first,
                           std::size_t count, float* dst)
    {
        auto* d = reinterpret_cast<std::uint8_t*> (dst);
        const std::uint8_t* p0 = planes + 0 * total + first;
        const std::uint8_t* p1 = planes + 1 * total + first;
        const std::uint8_t* p2 = planes + 2 * total + first;
        const std::uint8_t* p3 = planes + 3 * total + first;
        for (std::size_t i = 0; i < count; ++i)
        {
            d[4 * i + 0] = p0[i];
            d[4 * i + 1] = p1[i];
            d[4 * i + 2] = p2[i];
            d[4 * i + 3] = p3[i];
        }
    }

    // True if the JSON text [p, end) nests no deeper than `limit` — the rule of kMaxDepth: a scalar is 0, an
    // array or object 1 + the deepest of its members, counted AS WRITTEN: the brackets outside strings, before
    // anything is parsed. A member a later duplicate key replaces still counts, so the verdict is the text's
    // and every reader reaches it the same way, whichever duplicate its parser keeps. One pass, no recursion
    // and no tree: text too deep to follow is refused without being followed. On text that is not JSON the
    // count may be anything; the parse that comes next refuses it.
    bool depthWithin (const char* p, const char* end, std::size_t limit)
    {
        std::size_t depth = 0;
        while (p < end)
        {
            const char c = *p++;
            if (c == '"')
            {
                while (p < end && *p != '"')
                    p += (*p == '\\' && end - p > 1) ? 2 : 1;
                if (p < end)
                    ++p;   // the closing quote; an unterminated string ends the text
            }
            else if (c == '[' || c == '{')
            {
                if (++depth > limit)
                    return false;
            }
            else if ((c == ']' || c == '}') && depth > 0)
            {
                --depth;
            }
        }
        return true;
    }

    // refillWeights' walk without the filling: every placeholder must name one of the `numArrays` segments
    // whose u32 lengths sit at `lengths` (the check refill makes), and the floats they name — per
    // placeholder, so a segment named twice counts twice — must stay within `maxFloats`. `named` never
    // exceeds `maxFloats`, so the subtraction cannot wrap.
    bool placeholdersFit (const json& node, const std::uint8_t* lengths, std::size_t numArrays,
                          std::size_t maxFloats, std::size_t& named)
    {
        if (node.is_object())
        {
            for (auto it = node.begin(); it != node.end(); ++it)
            {
                if (it.key() == "weights" && it.value().is_number_integer())
                {
                    const auto idx = it.value().get<long long>();
                    if (idx < 0 || (unsigned long long) idx >= numArrays)
                        return false;
                    const std::size_t len = getU32 (lengths + 4 * (std::size_t) idx);
                    if (len > maxFloats - named)
                        return false;
                    named += len;
                }
                else if (! placeholdersFit (it.value(), lengths, numArrays, maxFloats, named))
                {
                    return false;
                }
            }
        }
        else if (node.is_array())
        {
            for (const auto& v : node)
                if (! placeholdersFit (v, lengths, numArrays, maxFloats, named))
                    return false;
        }
        return true;
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
        const auto* text = static_cast<const char*> (namJson);
        if (! depthWithin (text, text + n, kMaxDepth))   // what pack writes, unpack and decode read back
            return {};
        auto j = json::parse (text, text + n);

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

        if (! extractWeights (j, arrays))
            return {};
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
    if (fmt < 1 || fmt > kFormatVersion || codec != CodecStore || dtype != DtypeF32)   // only 1 and 2 are defined
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
            // Where size_t is 32 bits (wasm32, armv7), u32 lengths can wrap the sum back onto the payload
            // size, and the slicing below would then read far past `flat`.
            if (lengths[i] > (std::numeric_limits<std::size_t>::max)() - totalFloats) return {};
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
            for (const float f : flat)   // every payload float, named by a placeholder or not
                if (! finiteF32 (f))
                    return {};

            std::size_t o = 0;
            for (std::size_t i = 0; i < numArrays; ++i)
            {
                segs[i].assign (flat.begin() + (std::ptrdiff_t) o,
                                flat.begin() + (std::ptrdiff_t) (o + lengths[i]));
                o += lengths[i];
            }
        }

        if (! depthWithin (skeleton.data(), skeleton.data() + skeleton.size(), kMaxDepth))
            return {};
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

const std::vector<float>* Decoded::weightsOf (const json& node) const
{
    if (! node.is_object())
        return nullptr;
    const auto it = node.find ("weights");
    if (it == node.end() || ! it->is_number_integer())
        return nullptr;
    const auto idx = it->get<long long>();
    if (idx < 0 || idx >= (long long) segments.size())
        return nullptr;
    return &segments[(std::size_t) idx];
}

bool Decoded::refill (json& node) const
{
    bool ok = true;
    refillWeights (node, segments, ok);
    return ok;
}

// unpack's reading of the body, step for step, refusing what it refuses — minus the dump to text, plus
// the float cap. The stored floats meet the cap and the lengths meet the payload before anything is
// allocated; the skeleton (bounded by `n`) is parsed and its depth and placeholders checked before a
// single float is.
std::optional<Decoded> decode (const void* namz, std::size_t n, std::size_t maxFloats)
{
    if (! isNamz (namz, n) || n < 8)
        return std::nullopt;

    const auto* bytes = static_cast<const std::uint8_t*> (namz);
    const std::uint8_t fmt   = bytes[4];
    const std::uint8_t codec = bytes[5];
    const std::uint8_t dtype = bytes[6];
    const std::uint8_t flags = bytes[7];
    if (fmt < 1 || fmt > kFormatVersion || codec != CodecStore || dtype != DtypeF32)   // only 1 and 2 are defined
        return std::nullopt;

    std::size_t off = 8;
    if (fmt >= 2)
    {
        if (n < 10) return std::nullopt;
        off = 10 + (std::size_t) getU16 (bytes + 8);
        if (off > n) return std::nullopt;
    }

    const std::uint8_t* p = bytes + off;
    std::size_t rem = n - off;   // bytes still unread from `p`

    try
    {
        if (rem < 4) return std::nullopt;
        const std::size_t skeletonLen = getU32 (p); p += 4; rem -= 4;
        if (skeletonLen > rem) return std::nullopt;
        const char* skeleton = reinterpret_cast<const char*> (p);
        p += skeletonLen; rem -= skeletonLen;

        if (rem < 4) return std::nullopt;
        const std::size_t numArrays = getU32 (p); p += 4; rem -= 4;
        if (numArrays > rem / 4) return std::nullopt;   // can't hold that many u32 lengths
        const std::uint8_t* lengthBytes = p;
        p += 4 * numArrays; rem -= 4 * numArrays;

        // The float32 payload must be EXACTLY the rest of the buffer, and the floats it stores within the
        // cap — both known before anything is allocated. Each length is checked against what is left of the
        // payload, so the sum can neither overflow nor wrap — on a 32-bit size_t included. The lengths are
        // read where they lie, never copied out.
        if (rem % sizeof (float) != 0) return std::nullopt;
        const std::size_t totalFloats = rem / sizeof (float);
        if (totalFloats > maxFloats) return std::nullopt;
        const auto lengthOf = [lengthBytes] (std::size_t i) { return (std::size_t) getU32 (lengthBytes + 4 * i); };
        std::size_t summed = 0;
        for (std::size_t i = 0; i < numArrays; ++i)
        {
            const std::size_t len = lengthOf (i);
            if (len > totalFloats - summed) return std::nullopt;
            summed += len;
        }
        if (summed != totalFloats) return std::nullopt;

        if (! depthWithin (skeleton, skeleton + skeletonLen, kMaxDepth)) return std::nullopt;
        Decoded out;
        out.skeleton = json::parse (skeleton, skeleton + skeletonLen);
        std::size_t named = 0;
        if (! placeholdersFit (out.skeleton, lengthBytes, numArrays, maxFloats, named))
            return std::nullopt;

        out.segments.resize (numArrays);
        std::size_t first = 0;
        for (std::size_t i = 0; i < numArrays; ++i)
        {
            const std::size_t len = lengthOf (i);
            auto& seg = out.segments[i];
            seg.resize (len);
            if (len > 0)
            {
                if ((flags & FlagShuffle) != 0)
                    unshuffleSegment (p, totalFloats, first, len, seg.data());
                else
                    std::memcpy (seg.data(), p + first * sizeof (float), len * sizeof (float));
            }
            first += len;
        }
        for (const auto& seg : out.segments)   // the whole payload: the segments cover every float it stores
            for (const float f : seg)
                if (! finiteF32 (f))
                    return std::nullopt;
        return out;
    }
    catch (...) { return std::nullopt; }
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
        const auto* text = reinterpret_cast<const char*> (bytes + 10);
        if (! depthWithin (text, text + metaLen, kMaxDepth))
            return out;
        auto j = json::parse (text, text + metaLen);
        if (j.is_object())
            for (auto it = j.begin(); it != j.end(); ++it)
            {
                const auto& v = it.value();
                if      (v.is_string())          out[it.key()] = v.get<std::string>();
                else if (v.is_boolean())         out[it.key()] = v.get<bool>() ? "true" : "false";
                else if (v.is_number_unsigned()) out[it.key()] = std::to_string (v.get<unsigned long long>());   // past int64 too
                else if (v.is_number_integer())  out[it.key()] = std::to_string (v.get<long long>());
                else if (v.is_number())          out[it.key()] = std::to_string (v.get<double>());
                else                             out[it.key()] = v.dump();
            }
    }
    catch (...) {}
    return out;
}

} // namespace namz

#endif // NAMZ_IMPLEMENTATION
#endif // NAMZ_H
