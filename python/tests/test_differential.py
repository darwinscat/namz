# SPDX-License-Identifier: MIT
"""Optional differential test: Python pack(), unpack() and read_meta() must be byte-identical to the C++
reference CLI.

Skipped unless a built reference CLI is found (``$NAMZ_CLI`` or ``<repo>/build/namz``). This is the
ultimate byte-identity check — it runs the *actual* reference, not a golden. The committed conformance
vectors already cover the contract for CI; this adds a live cross-check for anyone with the C++ built.
"""

from __future__ import annotations

import json
import os
import random
import struct
import subprocess

import numpy as np
import pytest

import namz


def _find_cli():
    env = os.environ.get("NAMZ_CLI")
    if env and os.path.isfile(env):
        return env
    here = os.path.dirname(os.path.abspath(__file__))
    for up in range(5):
        cand = os.path.join(here, *([".."] * up), "build", "namz")
        if os.path.isfile(cand):
            return os.path.abspath(cand)
    return None


CLI = _find_cli()
pytestmark = pytest.mark.skipif(CLI is None, reason="reference CLI not built (set $NAMZ_CLI or build/namz)")


def _ref_encode(tmp_path, nam_bytes, shuffle, sets, f16=False):
    inp = tmp_path / "in.nam"
    out = tmp_path / ("out.nam16z" if f16 else "out.namz")
    inp.write_bytes(nam_bytes)
    if out.exists():
        out.unlink()
    args = [CLI, "encode", str(inp), str(out)]
    if f16:
        args.append("--f16")
    if not shuffle:
        args.append("--no-shuffle")
    for k, v in (sets or {}).items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True)
    return out.read_bytes() if (r.returncode == 0 and out.exists()) else None


def _check(tmp_path, nam_bytes, shuffle=True, sets=None, f16=False):
    ref = _ref_encode(tmp_path, nam_bytes, shuffle, sets, f16)
    dtype = namz.DTYPE_F16 if f16 else namz.DTYPE_F32
    mine = namz.pack(nam_bytes, namz.PackOptions(shuffle=shuffle, metadata=dict(sets or {}), dtype=dtype))
    assert (ref is None) == (mine == b""), f"accept/reject differs for {nam_bytes[:80]!r}"
    if ref is not None:
        assert ref == mine, f"byte mismatch for {nam_bytes[:80]!r}"


TARGETED = [
    b'{"sample_rate":48000.0,"weights":[1.0,2.0]}',
    b'{"x":0.1,"y":0.3333333333333333,"z":1e20,"w":1e-7,"weights":[0.5]}',
    # Integers past uint64/int64 are a double, as nlohmann holds them.
    b'{"x":18446744073709551616,"weights":[1.0]}',
    b'{"a":9223372036854775807,"b":18446744073709551615,"weights":[1.0]}',
    '{"ключ":"значение","emoji":"🎸","weights":[1.0]}'.encode(),
    b'{"s":"a\\tb\\nc\\u0001\\"\\\\","weights":[1.0]}',
    b'{"weights":[0,1,-1,16777216,-16777216]}',
    b'[1,2,3]',
    b'{"config":{"z":1,"a":2,"m":[{"weights":[1.0]},{"weights":[]}]},"weights":[9.0]}',
    # digits nlohmann's Grisu2 picks its own way, and its switch to exponent form past 15 integer digits
    b'{"a":-0.06020416246376235,"b":-198.70986938476562,"c":1e15,"d":2e15,"e":1e-5,"weights":[0.5]}',
    b'{"a":123456789012345678901234567890,"b":1.2345678901234567e300,"c":-4.9406564584124654e-324,"weights":[0.5]}',
    # refused before a duplicate key drops the value: a number past every double, a lone surrogate, depth
    b'{"x":1e400,"x":0,"weights":[0.5]}',
    b'{"x":' + b"9" * 400 + b',"x":0,"weights":[0.5]}',
    b'{"s":"\\ud800","s":"ok","weights":[0.5]}',
    b'{"x":' + b"[" * 128 + b"0" + b"]" * 128 + b',"x":0,"weights":[0.5]}',
    # a weight past the float32 range; UTF-16
    b'{"weights":[0.5,1e39]}',
    '{"weights":[0.5]}'.encode("utf-16-le"),
    # float16: the range edge, ties, subnormals, a zero that keeps its sign (pack writes it only with --f16)
    b'{"weights":[65504,65519.99609375,-65519,65520]}',
    b'{"weights":[65519.99609375,-65504.0,1.00048828125,1.00146484375,2047.5,2049]}',
    b'{"weights":[5.960464477539063e-08,2.9802322387695312e-08,-2.9802322387695312e-08,1e-8,-0.0,6.097555160522461e-05]}',
]


@pytest.mark.parametrize("nam", TARGETED)
@pytest.mark.parametrize("shuffle", [True, False])
@pytest.mark.parametrize("f16", [False, True])
def test_targeted_byte_identical(tmp_path, nam, shuffle, f16):
    _check(tmp_path, nam, shuffle=shuffle, f16=f16)


def test_metadata_byte_identical(tmp_path):
    _check(tmp_path, b'{"architecture":"X","weights":[0.5]}',
           sets={"tone_type": "hi-gain", "boost": "true", "n": "42", "device": "tube:1,pnp:1"})
    _check(tmp_path, b'{"architecture":"X","weights":[0.5]}',
           sets={"big": "99999999999999999999", "neg": "-5", "zeros": "007"})
    _check(tmp_path, b'[1,2,3]', sets={"foo": "bar"})  # both must reject (metadata on non-object)
    _check(tmp_path, b'{"architecture":"X","weights":[0.5]}', sets={"tone_type": "clean", "n": "7"}, f16=True)


def test_random_fuzz_byte_identical(tmp_path):
    rnd = random.Random(0xC0FFEE)

    def rf():
        r = rnd.random()
        if r < 0.4:
            return float(rnd.randint(-1_000_000, 1_000_000))
        if r < 0.7:
            return round(rnd.uniform(-1000, 1000), rnd.randint(1, 8))
        return rnd.choice([0.0, -0.0, 0.5, 1.0 / 3.0, -18.0, 48000.0])

    def robj(depth=0):
        o = {}
        for _ in range(rnd.randint(0, 4)):
            k = rnd.choice(["architecture", "sample_rate", "gain", "cfg", "zzz", "aaa"])
            r = rnd.random()
            if r < 0.35:
                o[k] = rnd.randint(-100000, 100000) if rnd.random() < 0.6 else rf()
            elif r < 0.55:
                o[k] = rnd.choice(["WaveNet", "LSTM", "X"])
            elif r < 0.65:
                o[k] = rnd.choice([True, False, None])
            elif r < 0.8 and depth < 3:
                o[k] = robj(depth + 1)
            else:
                o[k] = None
        if rnd.random() < 0.7:
            o["weights"] = [np.float32(rf()).item() for _ in range(rnd.randint(0, 40))]
        return o

    for _ in range(400):
        nam = json.dumps(robj(), separators=(",", ":")).encode()
        _check(tmp_path, nam, shuffle=rnd.random() < 0.5)

    # float16: the same objects with every weight brought into half's range (|w| <= 1e6 / 32), so they pack
    def tame(o):
        if isinstance(o.get("weights"), list):
            o["weights"] = [w / 32 for w in o["weights"]]
        for v in o.values():
            if isinstance(v, dict):
                tame(v)
        return o

    packed = 0
    for _ in range(400):
        nam = json.dumps(tame(robj()), separators=(",", ":")).encode()
        _check(tmp_path, nam, shuffle=rnd.random() < 0.5, f16=True)
        packed += namz.pack(nam, namz.PackOptions(dtype=namz.DTYPE_F16)) != b""
    assert packed > 300, "only %d of 400 float16 cases packed" % packed


def _ref_decode(tmp_path, blob):
    inp, out = tmp_path / "in.namz", tmp_path / "out.nam"
    inp.write_bytes(blob)
    if out.exists():
        out.unlink()
    r = subprocess.run([CLI, "decode", str(inp), str(out)], capture_output=True)
    return out.read_bytes() if (r.returncode == 0 and out.exists()) else None


def test_unpack_random_float32_byte_identical(tmp_path):
    # 100,000 random float32 bit patterns (no NaN or infinity) as one segment: the reference prints each
    # with Grisu2's digits, which are not always the shortest — about one in four hundred differs from repr
    rng = np.random.default_rng(0xF10A7)
    bits = rng.integers(0, 1 << 32, size=120_000, dtype=np.uint64).astype(np.uint32)
    bits = bits[(bits & 0x7F800000) != 0x7F800000][:100_000]
    floats = bits.view("<f4")
    skeleton = b'{"weights":0}'
    blob = (b"NAMZ" + bytes([2, 0, 0, 0, 0, 0]) + struct.pack("<I", len(skeleton)) + skeleton
            + struct.pack("<II", 1, floats.size) + floats.tobytes())
    ref = _ref_decode(tmp_path, blob)
    assert ref is not None
    assert namz.unpack(blob) == ref


def _ref_meta(tmp_path, blob):
    inp = tmp_path / "in.namz"
    inp.write_bytes(blob)
    r = subprocess.run([CLI, "map", str(inp), "--json"], capture_output=True)
    assert r.returncode == 0
    return json.loads(r.stdout)


@pytest.mark.parametrize("block", [
    b'{"a":[1,2.5],"b":{"y":1,"x":1e15},"c":[0.25354045629501343,"\xc3\xa9"],"d":null}',
    b'{"i":-9223372036854775808,"u":18446744073709551615,"d":18446744073709551616,"f":0.0078125,"g":-2.5e-7}',
    b'{"n":"x","big":' + b"9" * 400 + b"}",
    b'{"n":"x","big":1e400}',
    b'{"n":"\\ud800"}',
    b'{"a":' + b"[" * 128 + b"0" + b"]" * 128 + b',"a":0,"name":"n"}',
    '{"a":"b"}'.encode("utf-16-le"),
])
def test_read_meta_matches_the_reference(tmp_path, block):
    skeleton = b'{"weights":0}'
    blob = (b"NAMZ" + bytes([2, 0, 0, 0]) + struct.pack("<H", len(block)) + block
            + struct.pack("<I", len(skeleton)) + skeleton + struct.pack("<II", 1, 1) + struct.pack("<f", 0.5))
    assert namz.read_meta(blob) == _ref_meta(tmp_path, blob)


def _container16(halves, shuffle):
    """A float16 container: the halves little-endian, or every low byte then every high byte."""
    h = np.asarray(halves, dtype="<u2")
    payload = ((h & 0xFF).astype(np.uint8).tobytes() + (h >> 8).astype(np.uint8).tobytes()) if shuffle else h.tobytes()
    sk = b'{"weights":0}'
    return (b"NAMZ" + bytes([2, 0, 1, 1 if shuffle else 0, 0, 0]) + struct.pack("<I", len(sk)) + sk
            + struct.pack("<II", 1, h.size) + payload)


@pytest.mark.parametrize("shuffle", [True, False])
def test_unpack_of_every_finite_half_byte_identical(tmp_path, shuffle):
    finite = [h for h in range(0x10000) if (h >> 10) & 0x1F != 0x1F]
    blob = _container16(finite, shuffle)
    ref = _ref_decode(tmp_path, blob)
    assert ref is not None
    assert namz.unpack(blob) == ref


@pytest.mark.parametrize("bad", [0x7C00, 0xFC00, 0x7C01, 0x7E00, 0xFDFF, 0xFFFF])
def test_an_infinite_or_nan_half_refused_by_both(tmp_path, bad):
    blob = _container16([0x3C00, bad], True)
    assert _ref_decode(tmp_path, blob) is None
    assert namz.unpack(blob) == b""


@pytest.mark.parametrize("args", [
    ["verify", "a.nam", "--f16"],
    ["verify", "big.nam", "--f16"],
    ["verify", "a.nam", "--bogus"],
    ["encode", "a.nam", "x.namz", "--f16"],
    ["encode", "a.nam", "x.Nam16Z"],
    ["encode", "big.nam", "x.nam16z", "--f16"],
    ["map", "seed.nam16z"],
    ["map", "seed.nam16z", "--json"],
])
def test_cli_float16_lines_and_refusals_are_the_references(tmp_path, args):
    """The reference CLI and this one, same arguments, same directory: exit code, stdout, stderr whole."""
    import sys
    (tmp_path / "a.nam").write_bytes(
        b'{"architecture":"WaveNet","config":{"weights":[0.3,-0.7,12.5]},"weights":[0.1,-0.2,0.333,1e-6]}')
    (tmp_path / "big.nam").write_bytes(b'{"weights":[0.5,65520]}')
    (tmp_path / "seed.nam").write_bytes(b'{"architecture":"X","weights":[0.5]}')
    subprocess.run([CLI, "encode", "seed.nam", "seed.nam16z", "--f16", "--set", "tone_type=clean"],
                   cwd=str(tmp_path), capture_output=True, check=True)
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")
    env = dict(os.environ, PYTHONPATH=os.path.abspath(src) + os.pathsep + os.environ.get("PYTHONPATH", ""))
    ref = subprocess.run([CLI] + args, cwd=str(tmp_path), capture_output=True)
    mine = subprocess.run([sys.executable, "-m", "namz"] + args, cwd=str(tmp_path), capture_output=True, env=env)
    assert (mine.returncode, mine.stdout, mine.stderr) == (ref.returncode, ref.stdout, ref.stderr)
