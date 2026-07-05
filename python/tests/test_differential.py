# SPDX-License-Identifier: MIT
"""Optional differential test: Python pack() must be byte-identical to the C++ reference CLI.

Skipped unless a built reference CLI is found (``$NAMZ_CLI`` or ``<repo>/build/namz``). This is the
ultimate byte-identity check — it runs the *actual* reference, not a golden. The committed conformance
vectors already cover the contract for CI; this adds a live cross-check for anyone with the C++ built.
"""

from __future__ import annotations

import json
import os
import random
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


def _ref_encode(tmp_path, nam_bytes, shuffle, sets):
    inp = tmp_path / "in.nam"
    out = tmp_path / "out.namz"
    inp.write_bytes(nam_bytes)
    if out.exists():
        out.unlink()
    args = [CLI, "encode", str(inp), str(out)]
    if not shuffle:
        args.append("--no-shuffle")
    for k, v in (sets or {}).items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True)
    return out.read_bytes() if (r.returncode == 0 and out.exists()) else None


def _check(tmp_path, nam_bytes, shuffle=True, sets=None):
    ref = _ref_encode(tmp_path, nam_bytes, shuffle, sets)
    mine = namz.pack(nam_bytes, namz.PackOptions(shuffle=shuffle, metadata=dict(sets or {})))
    assert (ref is None) == (mine == b""), f"accept/reject differs for {nam_bytes[:80]!r}"
    if ref is not None:
        assert ref == mine, f"byte mismatch for {nam_bytes[:80]!r}"


TARGETED = [
    b'{"sample_rate":48000.0,"weights":[1.0,2.0]}',
    b'{"x":0.1,"y":0.3333333333333333,"z":1e20,"w":1e-7,"weights":[0.5]}',
    # Integers past uint64/int64 are coerced to double like nlohmann; exact for double-representable
    # magnitudes such as 2^64 (nlohmann's own parser rounds extreme 23+-digit ints imprecisely, but such
    # values never occur in NAM config — see _coerce_oversized_ints).
    b'{"x":18446744073709551616,"weights":[1.0]}',
    b'{"a":9223372036854775807,"b":18446744073709551615,"weights":[1.0]}',
    '{"ключ":"значение","emoji":"🎸","weights":[1.0]}'.encode(),
    b'{"s":"a\\tb\\nc\\u0001\\"\\\\","weights":[1.0]}',
    b'{"weights":[0,1,-1,16777216,-16777216]}',
    b'[1,2,3]',
    b'{"config":{"z":1,"a":2,"m":[{"weights":[1.0]},{"weights":[]}]},"weights":[9.0]}',
]


@pytest.mark.parametrize("nam", TARGETED)
@pytest.mark.parametrize("shuffle", [True, False])
def test_targeted_byte_identical(tmp_path, nam, shuffle):
    _check(tmp_path, nam, shuffle=shuffle)


def test_metadata_byte_identical(tmp_path):
    _check(tmp_path, b'{"architecture":"X","weights":[0.5]}',
           sets={"tone_type": "hi-gain", "boost": "true", "n": "42", "device": "tube:1,pnp:1"})
    _check(tmp_path, b'{"architecture":"X","weights":[0.5]}',
           sets={"big": "99999999999999999999", "neg": "-5", "zeros": "007"})
    _check(tmp_path, b'[1,2,3]', sets={"foo": "bar"})  # both must reject (metadata on non-object)


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
