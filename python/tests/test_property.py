# SPDX-License-Identifier: MIT
"""Local property fuzz: random finite float32 → round-trip → bit-exact + idempotent.

This is the per-language property test (not shared bytes): it hammers the codec with values the golden
vectors don't cover, in both shuffle modes and at nested depth.
"""

from __future__ import annotations

import json
from typing import List

import numpy as np
import pytest

import namz


def _random_finite_f32(rng: np.random.Generator, n: int) -> np.ndarray:
    """``n`` random float32 drawn uniformly over the 32-bit patterns, minus NaN/Inf."""
    bits = rng.integers(0, 1 << 32, size=n * 2, dtype=np.uint64).astype(np.uint32)
    vals = bits.view(np.float32)
    finite = vals[np.isfinite(vals)]
    while finite.size < n:  # extremely unlikely; top up if the filter took too many
        extra = rng.integers(0, 1 << 32, size=n, dtype=np.uint64).astype(np.uint32).view(np.float32)
        finite = np.concatenate([finite, extra[np.isfinite(extra)]])
    return finite[:n].astype(np.float32)


def _nam_with_weights(arrays: List[np.ndarray]) -> bytes:
    """Build a plausibly nested NAM JSON carrying the given weight arrays."""
    obj = {
        "architecture": "WaveNet",
        "sample_rate": 48000,
        "config": {
            "submodels": [
                {"max_value": 0.5, "model": {"architecture": "WaveNet", "weights": arrays[0].tolist()}}
            ]
        },
        "weights": arrays[1].tolist() if len(arrays) > 1 else [],
    }
    return json.dumps(obj).encode("utf-8")


@pytest.mark.parametrize("shuffle", [True, False])
@pytest.mark.parametrize("seed", range(12))
def test_roundtrip_bit_exact(seed: int, shuffle: bool):
    rng = np.random.default_rng(seed)
    a0 = _random_finite_f32(rng, int(rng.integers(0, 300)))
    a1 = _random_finite_f32(rng, int(rng.integers(0, 300)))
    src = _nam_with_weights([a0, a1])
    opts = namz.PackOptions(shuffle=shuffle)

    packed = namz.pack(src, opts)
    assert packed and namz.is_namz(packed)

    back = namz.unpack(packed)
    assert back

    obj = json.loads(back)
    got0 = np.asarray(obj["config"]["submodels"][0]["model"]["weights"], dtype=np.float32)
    got1 = np.asarray(obj["weights"], dtype=np.float32)
    assert got0.tobytes() == a0.tobytes(), "inner weights not bit-exact"
    assert got1.tobytes() == a1.tobytes(), "outer weights not bit-exact"

    # idempotence: re-pack the reconstruction must be byte-identical
    assert namz.pack(back, opts) == packed


def test_special_values_bit_exact():
    """-0.0, subnormals, FLT_MAX survive round-trip bit-exact."""
    specials = np.array(
        [0.0, -0.0, np.float32(1.401298464324817e-45), np.float32(1.1754943508222875e-38),
         np.float32(3.4028234663852886e38), np.float32(-3.4028234663852886e38), np.float32(1.0 / 3.0)],
        dtype=np.float32,
    )
    src = json.dumps({"architecture": "X", "weights": specials.tolist()}).encode("utf-8")
    for shuffle in (True, False):
        packed = namz.pack(src, namz.PackOptions(shuffle=shuffle))
        back = namz.unpack(packed)
        got = np.asarray(json.loads(back)["weights"], dtype=np.float32)
        assert got.tobytes() == specials.tobytes()
