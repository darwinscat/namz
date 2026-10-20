// SPDX-License-Identifier: MIT
// namz — command-line packer/unpacker for NeuralAmpModeler `.nam` <-> `.namz`.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.
// The SAME codec (namz.h) a host would embed, so the CLI output is byte-for-byte what a plugin consumes.

#define NAMZ_IMPLEMENTATION
#include "namz.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
constexpr std::size_t kMaxJson = 256u * 1024u * 1024u;   // reconstruction cap (zip-bomb guard)

std::vector<std::uint8_t> readFile (const char* path, bool& ok)
{
    std::ifstream f (path, std::ios::binary);
    if (! f) { ok = false; return {}; }
    std::vector<std::uint8_t> d ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    ok = true;
    return d;
}

bool writeFile (const char* path, const std::vector<std::uint8_t>& d)
{
    std::ofstream f (path, std::ios::binary);
    if (! f) return false;
    if (! d.empty())
        f.write (reinterpret_cast<const char*> (d.data()), (std::streamsize) d.size());
    return (bool) f;
}

// True if `path` ends in ".nam16z", ASCII case ignored — the name a float16 file goes by.
bool namedF16 (const std::string& path)
{
    static const char kExt[] = ".nam16z";
    const std::size_t n = sizeof (kExt) - 1;
    if (path.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i)
    {
        char c = path[path.size() - n + i];
        if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
        if (c != kExt[i]) return false;
    }
    return true;
}

int usage()
{
    std::fprintf (stderr,
        "namz — lossless .nam <-> .namz codec\n\n"
        "Usage:\n"
        "  namz encode <in.nam> <out.namz> [--no-shuffle] [--set key=value ...]\n"
        "  namz encode <in.nam> <out.nam16z> --f16 [--no-shuffle] [--set key=value ...]\n"
        "                                        float16 weights: half the size, LOSSY\n"
        "  namz decode <in.namz|in.nam16z> <out.nam>\n"
        "  namz map    <in.namz|in.nam16z> [--json]   print the metadata header (no weight decode)\n"
        "  namz verify <in.nam> [--f16]          pack->unpack round-trip check + ratio\n");
    return 2;
}

int doEncode (int argc, char** argv)
{
    if (argc < 4) return usage();
    const char* in = argv[2];
    const char* out = argv[3];
    namz::PackOptions opts;
    for (int i = 4; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--no-shuffle") { opts.shuffle = false; }
        else if (a == "--f16")   { opts.dtype = namz::DtypeF16; }
        else if (a == "--set" && i + 1 < argc)
        {
            std::string kv = argv[++i];
            const auto eq = kv.find ('=');
            if (eq == std::string::npos) { std::fprintf (stderr, "bad --set (need key=value): %s\n", kv.c_str()); return 2; }
            opts.metadata[kv.substr (0, eq)] = kv.substr (eq + 1);
        }
        else { std::fprintf (stderr, "unknown option: %s\n", a.c_str()); return usage(); }
    }
    // float16 is lossy, so it is asked for by the flag, and the name says it: --f16 writes only a .nam16z,
    // and a .nam16z is only ever written with --f16. The name never picks the dtype by itself.
    const bool f16 = opts.dtype == namz::DtypeF16;
    if (f16 != namedF16 (out))
    {
        std::fprintf (stderr, f16 ? "--f16 writes lossy float16 weights: name the output .nam16z, not %s\n"
                                  : "%s is named .nam16z: pass --f16 to write float16 weights (lossy)\n", out);
        return 2;
    }
    bool ok = false;
    auto src = readFile (in, ok);
    if (! ok) { std::fprintf (stderr, "cannot read %s\n", in); return 1; }
    auto packed = namz::pack (src.data(), src.size(), opts);
    if (packed.empty())
    {
        std::fprintf (stderr, f16 ? "encode failed (not valid NAM JSON, or a weight past float16's range: |w| >= 65520, rounds past 65504?): %s\n"
                                  : "encode failed (not valid NAM JSON?): %s\n", in);
        return 1;
    }
    if (! writeFile (out, packed)) { std::fprintf (stderr, "cannot write %s\n", out); return 1; }
    std::fprintf (stderr, "%s -> %s  (%zu -> %zu bytes, %.1f%%)\n", in, out, src.size(), packed.size(),
                  src.empty() ? 0.0 : 100.0 * (double) packed.size() / (double) src.size());
    return 0;
}

int doDecode (int argc, char** argv)
{
    if (argc < 4) return usage();
    const char* in = argv[2];
    const char* out = argv[3];
    if (argc > 4) { std::fprintf (stderr, "unknown option: %s\n", argv[4]); return usage(); }   // before anything is written
    bool ok = false;
    auto src = readFile (in, ok);
    if (! ok) { std::fprintf (stderr, "cannot read %s\n", in); return 1; }
    auto nam = namz::unpack (src.data(), src.size(), kMaxJson);
    if (nam.empty()) { std::fprintf (stderr, "decode failed (not a .namz / corrupt / over cap): %s\n", in); return 1; }
    if (! writeFile (out, nam)) { std::fprintf (stderr, "cannot write %s\n", out); return 1; }
    std::fprintf (stderr, "%s -> %s  (%zu -> %zu bytes)\n", in, out, src.size(), nam.size());
    return 0;
}

int doMap (int argc, char** argv)
{
    if (argc < 3) return usage();
    const char* in = argv[2];
    bool asJson = false;
    for (int i = 3; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--json") == 0) asJson = true;
        else { std::fprintf (stderr, "unknown option: %s\n", argv[i]); return usage(); }
    }
    bool ok = false;
    auto src = readFile (in, ok);
    if (! ok) { std::fprintf (stderr, "cannot read %s\n", in); return 1; }
    if (! namz::isNamz (src.data(), src.size())) { std::fprintf (stderr, "not a .namz: %s\n", in); return 1; }
    const std::map<std::string, std::string> m = namz::readMeta (src.data(), src.size());
    if (asJson)
    {
        // nlohmann's own print, keys sorted, no spaces: a control character in a value comes out escaped
        // (\t, \n, \u0001 …) and a NUL does not cut the line short, so this is JSON whatever the file holds.
        // Every key and value readMeta returns came out of nlohmann's parser, so each is valid UTF-8.
        const std::string line = nlohmann::json (m).dump();
        std::fwrite (line.data(), 1, line.size(), stdout);
        std::printf ("\n");
    }
    else
    {
        for (const auto& kv : m)
            std::printf ("  %s = %s\n", kv.first.c_str(), kv.second.c_str());
        if (m.empty())
            std::fprintf (stderr, "(no metadata header — v1 .namz or packed without --set)\n");
        if (src.size() > 6 && src[6] == namz::DtypeF16)
            std::fprintf (stderr, "(float16 weights — lossy)\n");
    }
    return 0;
}

int doVerify (int argc, char** argv)
{
    if (argc < 3) return usage();
    const char* in = argv[2];
    namz::PackOptions opts;
    for (int i = 3; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--f16") == 0) opts.dtype = namz::DtypeF16;
        else { std::fprintf (stderr, "unknown option: %s\n", argv[i]); return usage(); }
    }
    bool ok = false;
    auto src = readFile (in, ok);
    if (! ok) { std::fprintf (stderr, "cannot read %s\n", in); return 1; }
    auto packed = namz::pack (src.data(), src.size(), opts);
    if (packed.empty()) { std::fprintf (stderr, "FAIL pack: %s\n", in); return 1; }
    auto back = namz::unpack (packed.data(), packed.size(), kMaxJson);
    if (back.empty()) { std::fprintf (stderr, "FAIL unpack: %s\n", in); return 1; }
    // Idempotent + reversible: re-pack the reconstruction must byte-match the first pack.
    auto packed2 = namz::pack (back.data(), back.size(), opts);
    const bool idempotent = (packed == packed2);
    if (opts.dtype == namz::DtypeF16)
    {
        // What float16 cost: the largest change of any weight against the float32 pack, in absolute terms.
        const auto p32 = namz::pack (src.data(), src.size());
        const auto d32 = namz::decode (p32.data(), p32.size(), kMaxJson);
        const auto d16 = namz::decode (packed.data(), packed.size(), kMaxJson);
        if (! d32 || ! d16 || d32->segments.size() != d16->segments.size()) { std::fprintf (stderr, "FAIL decode: %s\n", in); return 1; }
        double worst = 0.0;
        for (std::size_t s = 0; s < d32->segments.size(); ++s)
            for (std::size_t k = 0; k < d32->segments[s].size(); ++k)
            {
                const double d = (double) d16->segments[s][k] - (double) d32->segments[s][k];
                worst = (std::max) (worst, d < 0 ? -d : d);
            }
        std::fprintf (stderr, "%s: %s  raw=%zu namz=%zu nam16z=%zu (%.1f%%)  idempotent=%s  max|w16-w32|=%.3g\n",
                      in, idempotent ? "OK" : "MISMATCH", src.size(), p32.size(), packed.size(),
                      src.empty() ? 0.0 : 100.0 * (double) packed.size() / (double) src.size(),
                      idempotent ? "yes" : "no", worst);
        return idempotent ? 0 : 1;
    }
    std::fprintf (stderr, "%s: %s  raw=%zu namz=%zu (%.1f%%)  idempotent=%s\n",
                  in, idempotent ? "OK" : "MISMATCH", src.size(), packed.size(),
                  src.empty() ? 0.0 : 100.0 * (double) packed.size() / (double) src.size(),
                  idempotent ? "yes" : "no");
    return idempotent ? 0 : 1;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "encode") return doEncode (argc, argv);
    if (cmd == "decode") return doDecode (argc, argv);
    if (cmd == "map")    return doMap (argc, argv);
    if (cmd == "verify") return doVerify (argc, argv);
    return usage();
}
