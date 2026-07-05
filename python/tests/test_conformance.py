# SPDX-License-Identifier: MIT
"""The cross-language TCK: every port must satisfy these shared vectors.

For each ``manifest["valid"]`` entry:
  1. encode(input, options) must **byte-match** the golden ``.namz``;
  2. decode(golden) must reproduce the weights **bit-exact** (float32);
  3. decode → encode must reproduce the golden (idempotence).
For each ``manifest["invalid"]`` entry: decode must be **rejected** (empty), never crash.
"""

from __future__ import annotations

import json
import os
from typing import Any, List

import numpy as np
import pytest

import namz


def _gather_weights(node: Any, out: List[np.ndarray]) -> None:
    """Independent reference: collect numeric ``"weights"`` arrays in sorted-DFS order as float32."""
    if isinstance(node, dict):
        for key in sorted(node.keys()):
            val = node[key]
            is_numeric = isinstance(val, list) and all(
                not isinstance(x, bool) and isinstance(x, (int, float)) for x in val
            )
            if key == "weights" and is_numeric:
                out.append(np.asarray(val, dtype=np.float32))
            else:
                _gather_weights(val, out)
    elif isinstance(node, list):
        for item in node:
            _gather_weights(item, out)


def _read(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def test_manifest_present(manifest):
    assert manifest["format"] == "namz"
    assert manifest["valid"] and manifest["invalid"]


def _valid_ids(manifest):
    return [c["name"] for c in manifest["valid"]]


def test_valid_vectors(manifest, conformance_dir):
    for case in manifest["valid"]:
        name = case["name"]
        src = _read(os.path.join(conformance_dir, case["input"]))
        expected = _read(os.path.join(conformance_dir, case["output"]))
        opts = namz.PackOptions(
            shuffle=case.get("shuffle", True), metadata=dict(case.get("set", {}))
        )

        # 1. encode must byte-match the golden
        got = namz.pack(src, opts)
        assert got == expected, (
            f"[{name}] encode byte mismatch ({len(got)} vs {len(expected)} bytes)"
        )

        # 2. decode must be bit-exact float32 vs the input weights
        decoded = namz.unpack(expected)
        assert decoded, f"[{name}] decode returned empty"
        want_w: List[np.ndarray] = []
        got_w: List[np.ndarray] = []
        _gather_weights(json.loads(src), want_w)
        _gather_weights(json.loads(decoded), got_w)
        assert len(want_w) == len(got_w), f"[{name}] weight-array count differs"
        for a, b in zip(want_w, got_w):
            assert a.tobytes() == b.astype(np.float32).tobytes(), (
                f"[{name}] weights not bit-exact"
            )

        # 3. decode → encode reproduces the golden (idempotence)
        assert namz.pack(decoded, opts) == expected, f"[{name}] not idempotent"


def test_invalid_vectors_rejected(manifest, conformance_dir):
    for case in manifest["invalid"]:
        blob = _read(os.path.join(conformance_dir, case["file"]))
        assert namz.unpack(blob) == b"", (
            f"[{case['name']}] should have been rejected: {case['why']}"
        )
