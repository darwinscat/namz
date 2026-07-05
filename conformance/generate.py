#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Generate the namz conformance vectors (the cross-language TCK) using the reference CLI.
# Usage: python3 conformance/generate.py [path-to-namz-cli]   (default: build/namz)
#
# Emits, relative to this file's directory:
#   vectors/<name>.nam   valid input
#   vectors/<name>.namz  reference output (from the CLI) — a port's encode must byte-match this
#   invalid/<name>.namz  malformed — a port's decode must reject it (error, never crash)
#   manifest.json        machine-readable description every port iterates
#
# Commit the outputs. Any language port validates against these files; if it diverges by one byte, it's wrong.

import json, os, struct, subprocess, sys, math

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "namz")
VEC = os.path.join(HERE, "vectors")
INV = os.path.join(HERE, "invalid")
os.makedirs(VEC, exist_ok=True)
os.makedirs(INV, exist_ok=True)

def f32(x):  # nearest float32, as a python float (so json emits a value that reparses to the same float32)
    return struct.unpack("f", struct.pack("f", x))[0]

FLT_MAX = 3.4028234663852886e+38
SUBNORMAL = 1.401298464324817e-45          # denorm_min
MIN_NORMAL = 1.1754943508222875e-38

# ---- valid cases: (name, json-object, options) ; options = {"shuffle": bool, "set": {k: v}} -----------
valid = [
    ("flat",            {"architecture": "WaveNet", "weights": [0.5, -0.5, 0.25]},                      {"shuffle": True}),
    ("flat-noshuffle",  {"architecture": "WaveNet", "weights": [0.5, -0.5, 0.25]},                      {"shuffle": False}),
    ("nested",          {"architecture": "SlimmableContainer", "sample_rate": 48000,
                         "config": {"submodels": [{"max_value": 0.5,
                             "model": {"architecture": "WaveNet", "weights": [0.1, -0.25, 12.5]}}]},
                         "weights": [1.0, -1.0]},                                                        {"shuffle": True}),
    ("metadata",        {"architecture": "WaveNet", "weights": [0.5]},
                        {"shuffle": True, "set": {"tone_type": "hi-gain", "boost": "true",
                                                  "device": "tube:1,pnp:1", "gear_make": "Two Notes"}}),
    ("no-weights",      {"architecture": "LSTM", "sample_rate": 44100, "metadata": {"loudness": -18.0}}, {"shuffle": True}),
    ("empty-weights",   {"architecture": "X", "weights": []},                                            {"shuffle": True}),
    ("edge-floats",     {"architecture": "X", "weights": [0.0, -0.0, f32(SUBNORMAL), f32(MIN_NORMAL),
                                                          f32(FLT_MAX), -f32(FLT_MAX), f32(1.0/3.0)]},   {"shuffle": True}),
    ("integer-weights", {"architecture": "X", "weights": [0, 1, -1, 16777216, -16777216]},               {"shuffle": True}),
    ("degenerate-array", [1, 2, 3],                                                                       {"shuffle": True}),
]

def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("CLI failed: %s\n%s" % (" ".join(args), r.stderr))

manifest = {"format": "namz", "note": "Cross-language conformance vectors. Encode a valid input with the "
            "given options and it must BYTE-MATCH the .namz; decode the .namz and the weights must be "
            "bit-exact float32. Every invalid .namz must be rejected (empty/error, never crash).",
            "valid": [], "invalid": []}

for name, obj, opts in valid:
    nam = os.path.join(VEC, name + ".nam")
    out = os.path.join(VEC, name + ".namz")
    with open(nam, "w") as f:
        json.dump(obj, f, separators=(",", ":"))
    args = [CLI, "encode", nam, out]
    if not opts.get("shuffle", True):
        args.append("--no-shuffle")
    for k, v in opts.get("set", {}).items():
        args += ["--set", "%s=%s" % (k, v)]
    run(args)
    manifest["valid"].append({"name": name, "input": "vectors/%s.nam" % name,
                              "output": "vectors/%s.namz" % name,
                              "shuffle": opts.get("shuffle", True), "set": opts.get("set", {})})

# ---- invalid cases: derive from a valid blob so they're realistic ------------------------------------
base = open(os.path.join(VEC, "flat.namz"), "rb").read()

def write_invalid(name, blob, why):
    open(os.path.join(INV, name + ".namz"), "wb").write(blob)
    manifest["invalid"].append({"name": name, "file": "invalid/%s.namz" % name, "why": why})

write_invalid("bad-magic",     b"XAMZ" + base[4:],                    "magic corrupted")
write_invalid("bad-version",   base[:4] + bytes([9]) + base[5:],      "formatVersion 9 (> 2) must be refused")
write_invalid("bad-codec",     base[:5] + bytes([1]) + base[6:],      "codec 1 (deflate, reserved) must be refused")
write_invalid("bad-dtype",     base[:6] + bytes([1]) + base[7:],      "dtype 1 (float16, reserved) must be refused")
write_invalid("truncated",     base[:len(base) - 3],                  "body truncated — exact-length check must reject")
write_invalid("trailing-junk", base + b"junk",                        "extra bytes — exact-length check must reject")
write_invalid("lying-metalen", base[:8] + b"\xff\xff" + base[10:],    "metaLen far bigger than the buffer")
write_invalid("empty",         b"",                                   "empty buffer")
write_invalid("magic-only",    b"NAMZ",                               "header too short")

with open(os.path.join(HERE, "manifest.json"), "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")

print("generated %d valid + %d invalid vectors" % (len(manifest["valid"]), len(manifest["invalid"])))
