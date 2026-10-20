#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Generate the namz conformance vectors (the cross-language TCK) using the reference CLI.
# Usage: python3 conformance/generate.py [path-to-namz-cli]   (default: build/namz)
#
# Emits, relative to this file's directory:
#   vectors/<name>.nam   valid input
#   vectors/<name>.namz  reference output (from the CLI) — a port's encode must byte-match this
#   invalid/<name>.namz  malformed — a port's decode must reject it (error, never crash)
#   decode-only/<name>.namz + .nam  a .namz pack never writes, and the exact .nam the reference decodes it to
#   invalid/<name>.nam   a .nam a port's encode must refuse
#   manifest.json        machine-readable description every port iterates
#
# Every verdict is the reference CLI's own: generation stops if the CLI accepts a blob listed as invalid,
# refuses a decode-only file, prints other bytes than the ones this script states, or encodes a refused .nam.
# A refusal is the CLI saying so — exit 1 and "encode failed" / "decode failed" — never a crash or any other
# exit, which stops generation too.
#
# Commit the outputs. Any language port validates against these files; if it diverges by one byte, it's wrong.

import json, os, struct, subprocess, sys, math, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "namz")
VEC = os.path.join(HERE, "vectors")
INV = os.path.join(HERE, "invalid")
DEC = os.path.join(HERE, "decode-only")
os.makedirs(VEC, exist_ok=True)
os.makedirs(INV, exist_ok=True)
os.makedirs(DEC, exist_ok=True)
_TMP = tempfile.TemporaryDirectory(prefix="namz-conformance-")   # where the CLI's verdicts on refused inputs land
TMP = _TMP.name

MAX_DEPTH = 128   # NAMZ-FORMAT.md "Placeholders and depth"


def depth_of(node):
    """The format's depth, measured without recursion: a scalar is 0, an array or object is 1 + its deepest
    member, so an empty one is 1."""
    deepest, todo = 0, [(node, 0)]
    while todo:
        n, above = todo.pop()
        if isinstance(n, (list, dict)):
            deepest = max(deepest, above + 1)
            todo.extend((m, above + 1) for m in (n.values() if isinstance(n, dict) else n))
    return deepest


def around(containers, innermost, objects):
    """`containers` levels around `innermost`: arrays only, or objects and arrays alternating outward."""
    node = innermost
    for i in range(containers):
        node = {"a": node} if objects and i % 2 == 0 else [node]
    return node


# A .nam exactly MAX_DEPTH deep along two branches: arrays around a scalar (a scalar is 0) and objects and
# arrays around an empty array (an empty one is 1). The weights sit at the top, so the stored skeleton is
# exactly as deep as the .nam: pack must take it and both readers must read it.
DEPTH_128 = {"architecture": "X", "deep": around(MAX_DEPTH - 1, 0, objects=False),
             "nest": around(MAX_DEPTH - 2, [], objects=True), "weights": [0.5]}
assert depth_of(DEPTH_128) == MAX_DEPTH and depth_of(DEPTH_128["deep"]) == depth_of(DEPTH_128["nest"]) == MAX_DEPTH - 1


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
    ("depth-128",       DEPTH_128,                                                                         {"shuffle": True}),
    # "weights" values that are NOT placeholders — a fraction, an exponent, an integer past uint64 — pack keeps
    # as the numbers they are, and the reference prints them back as 1.0, 0.0 and 1.8446744073709552e+19.
    # Raw text, because these literals are the point and Python's json cannot write 0e0.
    ("weights-not-placeholders",
                        b'{"a":{"weights":1.0},"architecture":"X","b":{"weights":0e0},'
                        b'"c":{"weights":18446744073709551616},"weights":[0.5]}',                         {"shuffle": True}),
    # Skeleton doubles whose digits the reference picks its own way (nlohmann's Grisu2: not always the shortest
    # digits, not always the closest), and the edges where it switches between fixed and exponent form. The
    # .namz stores the digits the reference prints, so a port's pack prints the same ones or fails the match.
    ("skeleton-doubles",
                        b'{"a":-0.06020416246376235,"b":-198.70986938476562,"c":1e15,"d":999999999999999.9,'
                        b'"e":1e16,"f":0.0001,"g":1e-05,"h":123456789012345.67,"i":5e-324,"j":1.7976931348623157e308,'
                        b'"architecture":"X","weights":[0.5]}',                                            {"shuffle": True}),
]

def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("CLI failed: %s\n%s" % (" ".join(args), r.stderr))


def cli_accepts(args):
    """True if the CLI does what `args` asks, False if it refuses with its own words; anything else — a crash,
    another exit code, another message — stops generation."""
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode == 0:
        return True
    if r.returncode == 1 and r.stderr.startswith(("encode failed", "decode failed")):
        return False
    sys.exit("the reference neither did nor refused: %s\nexit %d\n%s" % (" ".join(args), r.returncode, r.stderr))

manifest = {"format": "namz", "note": "Cross-language conformance vectors. Encode a valid input with the "
            "given options and it must BYTE-MATCH the .namz; decode the .namz and the weights must be "
            "bit-exact float32. Every invalid .namz must be rejected (empty/error, never crash). Decode each "
            "decodeOnly .namz and it must BYTE-MATCH its .nam. Encode each invalidNam .nam and it must be refused.",
            "valid": [], "invalid": [], "decodeOnly": [], "invalidNam": []}

for name, obj, opts in valid:
    nam = os.path.join(VEC, name + ".nam")
    out = os.path.join(VEC, name + ".namz")
    if isinstance(obj, bytes):
        with open(nam, "wb") as f:
            f.write(obj)
    else:
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
    path = os.path.join(INV, name + ".namz")
    open(path, "wb").write(blob)
    if cli_accepts([CLI, "decode", path, os.path.join(TMP, name + ".nam")]):
        sys.exit("the reference decodes invalid/%s.namz — it must refuse it: %s" % (name, why))
    manifest["invalid"].append({"name": name, "file": "invalid/%s.namz" % name, "why": why})


def craft(skeleton, lengths, payload_floats, version=2, floats=None):
    """A .namz written by hand: any skeleton, any lengths, `payload_floats` floats (0.5, 1.5, ...) of payload,
    stored unshuffled — the lengths need not sum to it. Version 1 has no metadata length. `floats`, if given,
    is the payload instead: each an int (the float32's bits) or a float."""
    out = b"NAMZ" + bytes([version, 0, 0, 0]) + (b"\0\0" if version >= 2 else b"")
    out += struct.pack("<I", len(skeleton)) + skeleton
    out += struct.pack("<I", len(lengths)) + b"".join(struct.pack("<I", n) for n in lengths)
    if floats is None:
        floats = [0.5 + i for i in range(payload_floats)]
    return out + b"".join(struct.pack("<I", x) if isinstance(x, int) else struct.pack("<f", x) for x in floats)


def nested(depth, innermost=b"0"):
    return b"[" * depth + innermost + b"]" * depth


def write_decode_only(name, blob, why, expect=None):
    path, out = os.path.join(DEC, name + ".namz"), os.path.join(DEC, name + ".nam")
    open(path, "wb").write(blob)
    run([CLI, "decode", path, out])
    got = open(out, "rb").read()
    if expect is not None and got != expect:
        sys.exit("the reference decodes decode-only/%s.namz to %r, not %r" % (name, got, expect))
    manifest["decodeOnly"].append({"name": name, "file": "decode-only/%s.namz" % name,
                                   "output": "decode-only/%s.nam" % name, "why": why})


def write_invalid_nam(name, text, why):
    path = os.path.join(INV, name + ".nam")
    open(path, "wb").write(text)
    if cli_accepts([CLI, "encode", path, os.path.join(TMP, name + ".namz")]):
        sys.exit("the reference encodes invalid/%s.nam — it must refuse it: %s" % (name, why))
    manifest["invalidNam"].append({"name": name, "file": "invalid/%s.nam" % name, "why": why})

write_invalid("bad-magic",     b"XAMZ" + base[4:],                    "magic corrupted")
write_invalid("bad-version",   base[:4] + bytes([9]) + base[5:],      "formatVersion 9 (> 2) must be refused")
write_invalid("bad-codec",     base[:5] + bytes([1]) + base[6:],      "codec 1 (deflate, reserved) must be refused")
write_invalid("bad-dtype",     base[:6] + bytes([1]) + base[7:],      "dtype 1 (float16, reserved) must be refused")
write_invalid("truncated",     base[:len(base) - 3],                  "body truncated — exact-length check must reject")
write_invalid("trailing-junk", base + b"junk",                        "extra bytes — exact-length check must reject")
write_invalid("lying-metalen", base[:8] + b"\xff\xff" + base[10:],    "metaLen far bigger than the buffer")
write_invalid("empty",         b"",                                   "empty buffer")
write_invalid("magic-only",    b"NAMZ",                               "header too short")

# ---- the stated rules (NAMZ-FORMAT.md "Placeholders and depth", "Wire format") -------------------------
# Every one of these blobs is a well-formed container but for the one thing its name says.
ONE = b'{"weights":0}'
write_invalid("version-zero",  craft(ONE, [1], 1, version=0),
              "formatVersion 0 — only 1 and 2 are defined; the same bytes with 1 are decode-only/version-1")
write_invalid("length-sum-wrap", craft(ONE, [2, 0xFFFFFFFF], 1),
              "lengths {2, 0xFFFFFFFF} over a one-float payload: the sum wraps to 1 in 32 bits, it is not 1")
write_invalid("placeholder-past-segments", craft(b'{"weights":1}', [1], 1),
              "placeholder 1 with one segment: names no segment")
write_invalid("placeholder-negative", craft(b'{"weights":-1}', [1], 1),
              "placeholder -1: names no segment")
write_invalid("placeholder-uint64", craft(b'{"weights":9223372036854775808}', [1], 1),
              "9223372036854775808 fits an unsigned 64-bit integer, so it is a placeholder, and it names no segment")
write_invalid("placeholder-duplicate-key", craft(b'{"weights":0,"weights":5}', [1], 1),
              "two \"weights\" keys in one object: the last one counts, and 5 names no segment")
for name, sk, why in (
        ("depth-129-scalar",  b'{"weights":0,"x":' + nested(128) + b"}",
         "129 deep: an object around 128 arrays around a scalar (a scalar is 0)"),
        ("depth-129-empty",   b'{"weights":0,"x":' + nested(127, b"[]") + b"}",
         "129 deep: an object around 127 arrays around an empty array (an empty one is 1)"),
        ("depth-129-objects", b'{"weights":0,"x":' + b'{"a":' * 127 + b"{}" + b"}" * 127 + b"}",
         "129 deep: an object around 127 objects around an empty object")):
    assert depth_of(json.loads(sk)) == MAX_DEPTH + 1, name
    write_invalid(name, craft(sk, [1], 1), why)
write_invalid("depth-10000", craft(b'{"weights":0,"x":' + nested(9999) + b"}", [1], 1),
              "10,000 deep: refused without following it, and nothing may run out of stack on the way")
write_invalid("depth-129-duplicate-key", craft(b'{"weights":0,"x":' + nested(128) + b',"x":0}', [1], 1),
              "129 deep as written: depth counts the text, so a member a later duplicate key replaces still counts")
write_invalid("skeleton-integer-overflow", craft(b'{"weights":0,"x":' + b"9" * 400 + b"}", [1], 1),
              "an integer literal past every double: not a number the reference can hold")
write_invalid("skeleton-utf16", craft(b'{"weights":0}'.decode().encode("utf-16-le"), [1], 1),
              "a skeleton in UTF-16: JSON text in a .namz is UTF-8")
write_invalid("skeleton-lone-surrogate", craft(b'{"s":"\\ud800","weights":0}', [1], 1),
              "an escaped lone surrogate: no Unicode text, so not a JSON string the reference reads")
write_invalid("payload-inf", craft(ONE, [1], 1, floats=[0x7F800000]),
              "a payload float that is +infinity: every payload float is a finite float32")
write_invalid("payload-nan", craft(ONE, [1, 1], 2, floats=[0x3F000000, 0x7FC00000]),
              "a payload float that is NaN, in a segment no placeholder names: the rule is the payload's")

write_decode_only("version-1", craft(ONE, [1], 1, version=1),
                  "formatVersion 1: no metadata length", b'{"weights":[0.5]}')
write_decode_only("placeholder-minus-zero", craft(b'{"weights":-0}', [1], 1),
                  "-0 is an integer literal, so it is a placeholder, and it names segment 0", b'{"weights":[0.5]}')
write_decode_only("placeholder-fraction", craft(b'{"weights":1.0}', [1, 1], 2),
                  "1.0 has a fraction: a number, not a placeholder (segment 1 exists and stays unnamed)",
                  b'{"weights":1.0}')
write_decode_only("placeholder-exponent", craft(b'{"weights":0e0}', [1], 1),
                  "0e0 has an exponent: a number, not a placeholder, printed as 0.0", b'{"weights":0.0}')
write_decode_only("placeholder-past-uint64", craft(b'{"weights":18446744073709551616}', [1], 1),
                  "past every 64-bit integer: a double, not a placeholder", b'{"weights":1.8446744073709552e+19}')
write_decode_only("placeholder-duplicate-key", craft(b'{"weights":5,"weights":0}', [1], 1),
                  "two \"weights\" keys in one object: the last one counts, and 0 names segment 0",
                  b'{"weights":[0.5]}')
# Payload floats whose digits the reference picks its own way (nlohmann's Grisu2 on the float's double), and
# 2^50, past the 15 digits it prints in fixed form.
write_decode_only("float-digits",
                  craft(ONE, [5], 5, floats=[0.25895649194717407, -0.25354045629501343, -198.70986938476562,
                                             2.0 ** 50, 0.17340469360351563]),
                  "float32 payload values printed with the reference's own digits",
                  b'{"weights":[0.25895649194717407,-0.25354045629501343,-198.70986938476563,'
                  b'1.125899906842624e+15,0.17340469360351563]}')

write_invalid_nam("weights-integer", b'{"architecture":"X","weights":7}',
                  "an integer \"weights\" would be stored as a placeholder naming a segment that does not exist")
write_invalid_nam("weights-integer-nested", b'{"architecture":"X","config":{"weights":0},"weights":[0.5]}',
                  "config.weights 0 would read back as the top-level weights array")
write_invalid_nam("weights-minus-zero", b'{"architecture":"X","weights":-0}',
                  "-0 is an integer literal, so it would be stored as a placeholder")
write_invalid_nam("depth-129", b'{"architecture":"X","weights":[0.5],"x":' + nested(128) + b"}",
                  "129 deep: pack refuses what no reader would read back")
write_invalid_nam("depth-129-duplicate-key",
                  b'{"architecture":"X","x":' + nested(128) + b',"x":0,"weights":[0.5]}',
                  "129 deep as written: depth counts the text, so a member a later duplicate key replaces still counts")
write_invalid_nam("utf16", b'{"architecture":"X","weights":[0.5]}'.decode().encode("utf-16-le"),
                  "a .nam in UTF-16: JSON text is UTF-8")
write_invalid_nam("lone-surrogate", b'{"architecture":"X","s":"\\udc00","weights":[0.5]}',
                  "an escaped lone surrogate: no Unicode text, so not a JSON string")
write_invalid_nam("weights-float32-overflow", b'{"architecture":"X","weights":[0.5,1e39]}',
                  "1e39 is a finite JSON number past the float32 range: the payload never stores an infinity")

with open(os.path.join(HERE, "manifest.json"), "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")

print("generated %d valid + %d invalid + %d decode-only vectors, %d refused inputs" % (
    len(manifest["valid"]), len(manifest["invalid"]), len(manifest["decodeOnly"]), len(manifest["invalidNam"])))
