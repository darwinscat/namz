// SPDX-License-Identifier: MIT
// namz — command-line packer/unpacker for NeuralAmpModeler `.nam` <-> `.namz`.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.
// The SAME codec (namz.h) a host would embed, so the CLI output is byte-for-byte what a plugin consumes.

#define NAMZ_IMPLEMENTATION
#include "namz.h"

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

int usage()
{
    std::fprintf (stderr,
        "namz — lossless .nam <-> .namz codec\n\n"
        "Usage:\n"
        "  namz encode <in.nam> <out.namz> [--no-shuffle] [--set key=value ...]\n"
        "  namz decode <in.namz> <out.nam>\n"
        "  namz map    <in.namz> [--json]        print the metadata header (no weight decode)\n"
        "  namz verify <in.nam>                  pack->unpack round-trip check + ratio\n");
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
        else if (a == "--set" && i + 1 < argc)
        {
            std::string kv = argv[++i];
            const auto eq = kv.find ('=');
            if (eq == std::string::npos) { std::fprintf (stderr, "bad --set (need key=value): %s\n", kv.c_str()); return 2; }
            opts.metadata[kv.substr (0, eq)] = kv.substr (eq + 1);
        }
        else { std::fprintf (stderr, "unknown option: %s\n", a.c_str()); return usage(); }
    }
    bool ok = false;
    auto src = readFile (in, ok);
    if (! ok) { std::fprintf (stderr, "cannot read %s\n", in); return 1; }
    auto packed = namz::pack (src.data(), src.size(), opts);
    if (packed.empty()) { std::fprintf (stderr, "encode failed (not valid NAM JSON?): %s\n", in); return 1; }
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
    bool asJson = (argc >= 4 && std::strcmp (argv[3], "--json") == 0);
    bool ok = false;
    auto src = readFile (in, ok);
    if (! ok) { std::fprintf (stderr, "cannot read %s\n", in); return 1; }
    if (! namz::isNamz (src.data(), src.size())) { std::fprintf (stderr, "not a .namz: %s\n", in); return 1; }
    const std::map<std::string, std::string> m = namz::readMeta (src.data(), src.size());
    if (asJson)
    {
        std::printf ("{");
        bool first = true;
        for (const auto& kv : m)
        {
            // minimal JSON string escaping for the values we emit
            auto esc = [] (const std::string& s) { std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o; };
            std::printf ("%s\"%s\":\"%s\"", first ? "" : ",", esc (kv.first).c_str(), esc (kv.second).c_str());
            first = false;
        }
        std::printf ("}\n");
    }
    else
    {
        for (const auto& kv : m)
            std::printf ("  %s = %s\n", kv.first.c_str(), kv.second.c_str());
        if (m.empty())
            std::fprintf (stderr, "(no metadata header — v1 .namz or packed without --set)\n");
    }
    return 0;
}

int doVerify (int argc, char** argv)
{
    if (argc < 3) return usage();
    const char* in = argv[2];
    bool ok = false;
    auto src = readFile (in, ok);
    if (! ok) { std::fprintf (stderr, "cannot read %s\n", in); return 1; }
    auto packed = namz::pack (src.data(), src.size());
    if (packed.empty()) { std::fprintf (stderr, "FAIL pack: %s\n", in); return 1; }
    auto back = namz::unpack (packed.data(), packed.size(), kMaxJson);
    if (back.empty()) { std::fprintf (stderr, "FAIL unpack: %s\n", in); return 1; }
    // Idempotent + reversible: re-pack the reconstruction must byte-match the first pack.
    auto packed2 = namz::pack (back.data(), back.size());
    const bool idempotent = (packed == packed2);
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
