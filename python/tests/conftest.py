# SPDX-License-Identifier: MIT
"""Shared pytest fixtures — locate the cross-language conformance vectors."""

from __future__ import annotations

import json
import os

import pytest


def _find_conformance() -> str:
    """Walk up from this file until we find ``conformance/manifest.json``.

    Works both in the monorepo (../../conformance) and in an unpacked sdist (../conformance).
    """
    here = os.path.dirname(os.path.abspath(__file__))
    d = here
    for _ in range(6):
        cand = os.path.join(d, "conformance", "manifest.json")
        if os.path.isfile(cand):
            return os.path.join(d, "conformance")
        d = os.path.dirname(d)
    pytest.skip("conformance/ vectors not found (packaged without them?)")
    raise AssertionError  # unreachable — keeps type checkers happy


@pytest.fixture(scope="session")
def conformance_dir() -> str:
    return _find_conformance()


@pytest.fixture(scope="session")
def manifest(conformance_dir: str) -> dict:
    with open(os.path.join(conformance_dir, "manifest.json"), "rb") as f:
        return json.load(f)
