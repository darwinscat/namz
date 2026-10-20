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
import time
import warnings

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
    assert namz.unpack(FULL[:4] + bytes([9]) + FULL[5:]) == b"", "version 9 accepted"
    assert namz.unpack(FULL[:5] + bytes([1]) + FULL[6:]) == b"", "codec 1 (deflate) accepted"
    # dtype 1 is float16: a float32 payload is twice the halves its lengths ask, so the length rule refuses it
    assert namz.unpack(FULL[:6] + bytes([1]) + FULL[7:]) == b"", "dtype 1 on a float32 payload accepted"
    assert namz.unpack(FULL[:6] + bytes([2]) + FULL[7:]) == b"", "dtype 2 accepted"


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


# -- the stated rules (NAMZ-FORMAT.md "Placeholders and depth"), held to the reference's own tests -------
def _nested(depth: int, inner: str = "0") -> str:
    return "[" * depth + inner + "]" * depth


def _craft(skeleton: str, lengths, floats: int, version: int = 2, meta: "str | bytes" = "") -> bytes:
    """A container written by hand: any skeleton, any lengths, ``floats`` floats (0.5, 1.5, ...) of
    unshuffled payload — the lengths need not sum to it. Version 1 has no metadata block."""
    sk, mb = skeleton.encode(), (meta if isinstance(meta, bytes) else meta.encode())
    out = b"NAMZ" + bytes([version, 0, 0, 0])
    if version >= 2:
        out += struct.pack("<H", len(mb)) + mb
    out += struct.pack("<I", len(sk)) + sk + struct.pack("<I", len(lengths))
    out += b"".join(struct.pack("<I", n) for n in lengths)
    return out + b"".join(struct.pack("<f", 0.5 + i) for i in range(floats))


def test_max_depth_is_the_references_kmaxdepth():
    assert namz.MAX_DEPTH == 128


def test_depth_128_read_129_refused_by_pack_unpack_and_read_meta():
    def nam(d):
        return '{"weights":[1.5],"x":%s}' % _nested(d - 1)

    assert namz.unpack(namz.pack(nam(128))) == nam(128).encode(), "pack takes a .nam 128 deep and it reads back"
    assert namz.pack(nam(129)) == b"", "pack refuses a .nam 129 deep"

    def sk(d):  # an object around arrays around an empty array (an empty one is 1)
        return '{"weights":0,"x":%s}' % _nested(d - 2, "[]")

    assert namz.unpack(_craft(sk(128), [1], 1)), "unpack reads a skeleton 128 deep"
    assert namz.unpack(_craft(sk(129), [1], 1)) == b"", "unpack refuses a skeleton 129 deep"
    one = '{"weights":0}'
    assert list(namz.read_meta(_craft(one, [1], 1, meta='{"a":%s}' % _nested(127)))) == ["a"]
    assert namz.read_meta(_craft(one, [1], 1, meta='{"a":%s}' % _nested(128))) == {}, "a meta block 129 deep"


def test_depth_100000_refused_never_raised():
    assert namz.pack('{"weights":[1.5],"x":%s}' % _nested(99999)) == b""
    assert namz.pack(('{"weights":[1.5],"x":%s}' % _nested(99999)).encode()) == b""
    assert namz.unpack(_craft('{"weights":0,"x":%s}' % _nested(99999), [1], 1)) == b""
    assert namz.read_meta(_craft('{"weights":0}', [1], 1, meta='{"a":%s}' % _nested(32000))) == {}


def test_text_scans_are_linear_on_an_unterminated_string():
    # a quote, then escaped quotes and never a closing one, then more brackets than the fast path takes: a
    # scan that retried the string from every quote after it would be quadratic — about ten seconds at this
    # size, against a few milliseconds. The second text sends the lone-surrogate scan down the same road.
    for blob in ('"' + '\\"' * 30000 + "[" * 200, '"' + '\\u0041\\"' * 30000 + "[" * 200):
        start = time.perf_counter()
        assert namz.pack(blob.encode()) == b""
        assert time.perf_counter() - start < 1.0, "a text scan went quadratic"


def test_depth_counts_a_member_a_later_duplicate_key_replaces():
    deep = _nested(128)  # 129 deep inside the object, as written
    assert namz.pack('{"x":%s,"x":0,"weights":[0.5]}' % deep) == b""
    assert namz.unpack(_craft('{"weights":0,"x":%s,"x":0}' % deep, [1], 1)) == b""
    assert namz.read_meta(_craft('{"weights":0}', [1], 1, meta='{"a":%s,"a":0,"name":"n"}' % deep)) == {}


def test_a_value_a_later_duplicate_key_replaces_is_still_read():
    # nlohmann refuses the text before it drops a duplicate: a number past every double, a lone surrogate
    for nam in ('{"x":1e400,"x":0,"weights":[0.5]}', '{"x":%s,"x":0,"weights":[0.5]}' % ("9" * 400),
                '{"s":"\\ud800","s":"ok","weights":[0.5]}'):
        assert namz.pack(nam) == b"", nam


def test_json_text_is_utf8():
    nam = '{"architecture":"X","weights":[0.5]}'
    for enc in ("utf-16-le", "utf-16-be", "utf-16", "utf-32-le", "utf-32"):
        assert namz.pack(nam.encode(enc)) == b"", enc
    # one UTF-8 byte-order mark is skipped, as nlohmann skips it
    assert namz.pack(b"\xef\xbb\xbf" + nam.encode()) == namz.pack(nam.encode()) != b""
    one = '{"weights":0}'
    assert namz.read_meta(_craft(one, [1], 1, meta='{"a":"b"}'.encode("utf-16-le"))) == {}
    assert namz.read_meta(_craft(one, [1], 1, meta=b"\xef\xbb\xbf" + b'{"a":"b"}')) == {"a": "b"}


def test_pack_refuses_a_weight_past_float32_without_a_warning():
    with warnings.catch_warnings():
        warnings.simplefilter("error")  # numpy's "overflow encountered in cast" would raise here
        assert namz.pack('{"weights":[0.5,1e39]}') == b""
        assert namz.pack('{"a":{"weights":[-3.5e38]},"weights":[0.5]}') == b""
    assert namz.pack('{"weights":[3.4028235e38]}') != b"", "rounds to FLT_MAX: kept"


def test_read_meta_reads_what_the_reference_reads():
    # every expected value here is what the reference CLI's `map --json` prints for the same block
    def meta(block):
        return namz.read_meta(_craft('{"weights":0}', [1], 1, meta=block))

    assert meta('{"a":[1,2.5],"b":{"y":1,"x":1e15},"c":[0.25354045629501343,"\u00e9"]}') == {
        "a": "[1,2.5]", "b": '{"x":1e+15,"y":1}', "c": '[0.25354045629501343,"\u00e9"]'}
    assert meta('{"i":-9223372036854775808,"u":18446744073709551615,"d":18446744073709551616,"f":0.0078125}') == {
        "i": "-9223372036854775808", "u": "18446744073709551615", "d": "18446744073709551616.000000",
        "f": "0.007812"}
    # what the reference cannot read is no metadata: a number past every double, a lone surrogate
    for block in ('{"n":"x","big":%s}' % ("9" * 400), '{"n":"x","big":1e400}', '{"n":"\\ud800"}',
                  '{"n":"x","s":"\\udc00","s":"y"}'):
        assert meta(block) == {}, block


def test_pack_refuses_an_integer_weights_member_anywhere():
    for nam in (
        '{"weights":7}',
        '{"architecture":"X","config":{"weights":0},"weights":[0.5]}',
        '{"weights":-0}',
        '{"weights":18446744073709551615}',
        '{"a":[{"b":{"weights":-3}}],"weights":[1.5]}',
    ):
        assert namz.pack(nam) == b"", nam
    stamped = namz.PackOptions(metadata={"weights": "7"})  # typed to the integer 7 on the way in
    assert namz.pack('{"weights":[0.5]}', stamped) == b"", "metadata weights=7"


def test_pack_keeps_a_weights_that_is_no_integer_literal():
    for nam, back in (
        ('{"weights":1.0}', '{"weights":1.0}'),
        ('{"weights":0e0}', '{"weights":0.0}'),
        ('{"weights":18446744073709551616}', '{"weights":1.8446744073709552e+19}'),
        ('{"weights":"7"}', '{"weights":"7"}'),
        ('{"weights":true}', '{"weights":true}'),
        ('{"weights":null}', '{"weights":null}'),
        ('{"weights":{"a":1}}', '{"weights":{"a":1}}'),
        ('{"weights":["a",1]}', '{"weights":["a",1]}'),
    ):
        assert namz.unpack(namz.pack(nam)) == back.encode(), nam


def test_format_version_1_and_2_read_0_refused():
    one = '{"weights":0}'
    assert namz.unpack(_craft(one, [1], 1, version=1)) == b'{"weights":[0.5]}'
    assert namz.unpack(_craft(one, [1], 1, version=2)) == b'{"weights":[0.5]}'
    assert namz.unpack(_craft(one, [1], 1, version=0)) == b"", "version 0 accepted"
