# SPDX-License-Identifier: MIT
"""Falsification suite — actively try to BREAK the codec (mirrors tests/adversarial.cpp).

The bar is not "prove it works" but "try to make it crash, hang, silently corrupt, or accept a lie."
Invariants under attack:
  * ``unpack`` NEVER raises, for any bytes whatsoever.
  * Any non-empty ``unpack`` output is a **fixed point**: ``unpack(pack(X)) == X``.
  * A stored format has no self-healing: EVERY truncation and EVERY trailing byte must be rejected.
  * Header-critical bytes (magic / version / codec / dtype) must reject when corrupted.
  * Lies in the length fields (skeleton_len, numArrays, per-array lengths, metaLen, weight index)
    must be rejected, never trusted into an over-read.
"""

from __future__ import annotations

import json
import os
import struct

import numpy as np
import pytest

import namz

FULL = None  # a valid, shuffled .namz with a real payload (loaded from the conformance vectors)


@pytest.fixture(scope="module", autouse=True)
def _load_full(conformance_dir):
    global FULL
    with open(os.path.join(conformance_dir, "vectors", "flat.namz"), "rb") as f:
        FULL = f.read()
    assert namz.unpack(FULL), "sanity: flat.namz must decode"


# --------------------------------------------------------------------------------------------------
def test_unpack_never_raises_on_garbage():
    rng = np.random.default_rng(1234)
    for _ in range(2000):
        n = int(rng.integers(0, 64))
        blob = rng.integers(0, 256, size=n, dtype=np.uint8).tobytes()
        # must not raise; return type is always bytes
        assert isinstance(namz.unpack(blob), bytes)
    # also feed near-valid blobs (right magic, random tail)
    for _ in range(2000):
        n = int(rng.integers(0, 80))
        blob = b"NAMZ" + rng.integers(0, 256, size=n, dtype=np.uint8).tobytes()
        assert isinstance(namz.unpack(blob), bytes)


def test_pack_never_raises_on_garbage():
    rng = np.random.default_rng(99)
    for _ in range(1000):
        n = int(rng.integers(0, 64))
        blob = rng.integers(0, 256, size=n, dtype=np.uint8).tobytes()
        assert namz.pack(blob) == b"" or namz.is_namz(namz.pack(blob))


def test_every_truncation_rejected():
    # A stored format: any proper prefix must fail the exact-length check.
    for L in range(len(FULL)):
        assert namz.unpack(FULL[:L]) == b"", f"prefix of length {L} was wrongly accepted"


def test_every_trailing_extension_rejected():
    for k in (1, 2, 3, 4, 5, 7, 16, 64, 4096):
        assert namz.unpack(FULL + b"\x00" * k) == b"", f"+{k} trailing zero bytes accepted"
        assert namz.unpack(FULL + (b"junk!" * k)[:k]) == b"", f"+{k} trailing junk bytes accepted"


def test_header_critical_bytes_reject_when_corrupted():
    # magic (0..3), version (4), codec (5), dtype (6) — corrupting any must reject.
    for pos in (0, 1, 2, 3, 4, 5, 6):
        b = bytearray(FULL)
        b[pos] ^= 0xFF
        assert namz.unpack(bytes(b)) == b"", f"corrupting header byte {pos} was accepted"
    # explicit reserved values
    assert namz.unpack(FULL[:5] + bytes([9]) + FULL[6:]) == b"", "version 9 accepted"
    assert namz.unpack(FULL[:5] + bytes([1]) + FULL[6:]) == b"", "codec 1 (deflate) accepted"
    assert namz.unpack(FULL[:6] + bytes([1]) + FULL[7:]) == b"", "dtype 1 (float16) accepted"


def test_single_byte_flips_are_self_consistent():
    """Any accepted mutation must be a fixed point of pack∘unpack (no silent corruption)."""
    for pos in range(len(FULL)):
        for mask in (0x01, 0x80, 0xFF):
            b = bytearray(FULL)
            b[pos] ^= mask
            out = namz.unpack(bytes(b))
            assert isinstance(out, bytes)
            if out:
                # valid JSON, and a stable fixed point
                json.loads(out)  # must not raise
                assert namz.unpack(namz.pack(out)) == out, f"pos {pos} mask {mask:#x} not a fixed point"


def test_lying_meta_len_rejected():
    # metaLen far larger than the buffer → off > n → reject
    assert namz.unpack(FULL[:8] + b"\xff\xff" + FULL[10:]) == b""
    # metaLen that eats into (but not past) the body → body no longer parses / length check fails
    for ml in range(1, 40):
        blob = FULL[:8] + struct.pack("<H", ml) + FULL[10:]
        # never raises; if accepted it must be self-consistent
        out = namz.unpack(blob)
        assert isinstance(out, bytes)
        if out:
            assert namz.unpack(namz.pack(out)) == out


def _synthesize(skeleton_obj, num_arrays, lengths, payload, *, meta=b"", flags=0):
    """Hand-build a .namz body with attacker-chosen fields (bypasses pack)."""
    skel = json.dumps(skeleton_obj, sort_keys=True, separators=(",", ":")).encode("utf-8")
    body = bytearray()
    body += struct.pack("<I", len(skel)) + skel
    body += struct.pack("<I", num_arrays)
    for ln in lengths:
        body += struct.pack("<I", ln)
    body += payload
    out = bytearray(b"NAMZ" + bytes([2, 0, 0, flags]))
    out += struct.pack("<H", len(meta)) + meta
    out += body
    return bytes(out)


def test_lying_num_arrays_rejected():
    # numArrays claims 2^32-1 arrays — the length table can't fit; must reject, not allocate.
    blob = _synthesize({"weights": 0}, 0xFFFFFFFF, [], b"")
    assert namz.unpack(blob) == b""


def test_lying_lengths_rejected():
    # one array claiming 1000 floats but only 1 float of payload
    blob = _synthesize({"weights": 0}, 1, [1000], struct.pack("<f", 1.5))
    assert namz.unpack(blob) == b""
    # length sum correct but payload not a multiple of 4
    blob2 = _synthesize({"weights": 0}, 1, [1], b"\x00\x00\x00")
    assert namz.unpack(blob2) == b""


def test_out_of_range_weight_index_rejected():
    # skeleton references array #5 but zero arrays exist → corrupt stream
    blob = _synthesize({"weights": 5}, 0, [], b"")
    assert namz.unpack(blob) == b""
    # negative-looking index can't occur (json ints), but a valid index with no payload for it:
    blob2 = _synthesize({"a": {"weights": 1}, "weights": 0}, 1, [1], struct.pack("<f", 2.0))
    assert namz.unpack(blob2) == b""  # index 1 out of range (only 1 array)


def test_corrupt_skeleton_json_rejected():
    # valid framing, but the skeleton bytes are not JSON
    skel = b"{not json"
    body = struct.pack("<I", len(skel)) + skel + struct.pack("<I", 0)
    blob = b"NAMZ" + bytes([2, 0, 0, 0]) + struct.pack("<H", 0) + body
    assert namz.unpack(blob) == b""


def test_nan_inf_inputs_rejected_by_pack():
    # non-finite literals are out of contract; pack must reject them like the reference does.
    assert namz.pack(b'{"architecture":"X","weights":[NaN]}') == b""
    assert namz.pack(b'{"architecture":"X","weights":[Infinity]}') == b""
    assert namz.pack(b'{"architecture":"X","weights":[-Infinity]}') == b""
    assert namz.pack(b"not json at all") == b""
    assert namz.pack(b"") == b""


def test_overflow_weights_rejected_not_crash():
    # a decimal that overflows float32 to +Inf — pack must reject cleanly, never emit an Inf payload.
    assert namz.pack(b'{"architecture":"X","weights":[1e400]}') == b""
    assert namz.pack(b'{"architecture":"X","weights":[-1e400]}') == b""
    # an integer weight far too big to convert to float — must not raise OverflowError out of pack.
    huge = b'{"architecture":"X","weights":[' + b"9" * 400 + b"]}"
    assert namz.pack(huge) == b""


def test_overflow_non_weight_number_rejected():
    # a non-finite value in a NON-weight field must also be refused (skeleton would be non-standard JSON).
    assert namz.pack(b'{"architecture":"X","gain":1e400,"weights":[0.5]}') == b""


def test_huge_digit_metadata_kept_as_string():
    # a metadata value longer than int64 can hold must stay a string, never raise (int_max_str_digits).
    big = "9" * 4301
    blob = namz.pack(b'{"architecture":"X","weights":[1.0]}', namz.PackOptions(metadata={"n": big}))
    assert blob, "pack must not fail on an over-long digit string"
    assert namz.read_meta(blob) == {"n": big}


def test_nonfinite_payload_rejected_on_decode():
    # a crafted .namz whose float payload is NaN/Inf must be refused, not decoded to invalid JSON.
    for bad in (b"\x00\x00\xc0\x7f", b"\x00\x00\x80\x7f", b"\x00\x00\x80\xff"):  # NaN, +Inf, -Inf
        blob = _synthesize({"weights": 0}, 1, [1], bad)  # flags=0 → payload read as raw <f4
        assert namz.unpack(blob) == b"", "non-finite payload float was accepted"


def test_nonfinite_literal_in_skeleton_rejected_on_decode():
    # attacker-controlled skeleton with a NaN literal in a non-weight field → reject on re-serialize.
    skel = b'{"x":NaN}'
    body = struct.pack("<I", len(skel)) + skel + struct.pack("<I", 0)
    blob = b"NAMZ" + bytes([2, 0, 0, 0]) + struct.pack("<H", 0) + body
    assert namz.unpack(blob) == b""


def test_max_json_bytes_cap_enforced():
    # a legitimate blob whose reconstruction exceeds a tiny cap must be refused.
    big = json.dumps({"architecture": "X", "weights": list(range(5000))}).encode()
    packed = namz.pack(big)
    assert packed
    assert namz.unpack(packed, max_json_bytes=64) == b"", "over-cap reconstruction not refused"
    assert namz.unpack(packed, max_json_bytes=namz.DEFAULT_MAX_JSON_BYTES), "under cap should pass"


def test_shuffle_flag_mismatch_changes_values_not_stability():
    # Flipping the shuffle flag makes the payload decode to different (wrong) floats — but must remain
    # a clean, self-consistent decode, never a crash.
    b = bytearray(FULL)
    b[7] ^= 0x01  # toggle the shuffle flag bit
    out = namz.unpack(bytes(b))
    assert isinstance(out, bytes)
    if out:
        assert namz.unpack(namz.pack(out)) == out
