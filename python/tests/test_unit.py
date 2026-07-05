# SPDX-License-Identifier: MIT
"""Idiomatic unit tests for the public API and the CLI."""

from __future__ import annotations

import json
import os

import pytest

import namz
from namz import cli


# -- API -------------------------------------------------------------------------------------------
def test_is_namz():
    assert namz.is_namz(b"NAMZ....")
    assert not namz.is_namz(b"XAMZ")
    assert not namz.is_namz(b"NAM")
    assert not namz.is_namz(b"")


def test_pack_accepts_str_and_bytes():
    a = namz.pack('{"architecture":"X","weights":[0.5]}')
    b = namz.pack(b'{"architecture":"X","weights":[0.5]}')
    assert a and a == b


def test_roundtrip_minifies_and_sorts_keys():
    src = b'{"b":2,"architecture":"X","weights":[1.0,2.0],"a":1}'
    out = namz.unpack(namz.pack(src))
    # keys sorted ascending, minified, weights rehydrated as float
    assert out == b'{"a":1,"architecture":"X","b":2,"weights":[1.0,2.0]}'


def test_empty_weights_array_is_preserved():
    src = b'{"architecture":"X","weights":[]}'
    packed = namz.pack(src)
    assert namz.unpack(packed) == b'{"architecture":"X","weights":[]}'


def test_leading_zero_metadata_typed_as_int_like_stoll():
    # std::stoll ignores leading zeros and types by value, not string length.
    blob = namz.pack(
        b'{"architecture":"X","weights":[1.0]}',
        namz.PackOptions(metadata={"count": "000000000000000000001", "zeros": "0000"}),
    )
    assert namz.read_meta(blob) == {"count": "1", "zeros": "0"}
    assert b'"count":1' in namz.unpack(blob)  # integer in the skeleton, not a string
    # genuinely-too-big stays a string
    big = namz.pack(
        b'{"architecture":"X","weights":[1.0]}',
        namz.PackOptions(metadata={"n": "99999999999999999999"}),
    )
    assert namz.read_meta(big) == {"n": "99999999999999999999"}


def test_read_meta_typed_fields():
    src = b'{"architecture":"WaveNet","weights":[0.5]}'
    opts = namz.PackOptions(
        metadata={"tone_type": "hi-gain", "boost": "true", "count": "42", "device": "tube:1"}
    )
    blob = namz.pack(src, opts)
    meta = namz.read_meta(blob)
    assert meta == {"tone_type": "hi-gain", "boost": "true", "count": "42", "device": "tube:1"}
    # and the typing landed in the skeleton too
    obj = json.loads(namz.unpack(blob))
    assert obj["metadata"]["boost"] is True
    assert obj["metadata"]["count"] == 42
    assert obj["metadata"]["tone_type"] == "hi-gain"


def test_read_meta_empty_for_non_namz_and_v1():
    assert namz.read_meta(b"not a namz") == {}
    # v1 blob (formatVersion 1, no meta block) → empty
    v1 = b"NAMZ" + bytes([1, 0, 0, 0]) + b"..."
    assert namz.read_meta(v1) == {}
    # v2 with no metadata → empty
    assert namz.read_meta(namz.pack(b'{"architecture":"X","weights":[1.0]}')) == {}


def test_pack_without_metadata_has_no_header_block():
    blob = namz.pack(b'{"architecture":"X","weights":[1.0]}')
    # metaLen (u16 at offset 8) must be zero
    assert blob[8:10] == b"\x00\x00"


def test_default_options_shuffle_on():
    assert namz.PackOptions().shuffle is True
    assert namz.PackOptions().metadata == {}


def test_oversized_ints_coerced_to_double_like_reference():
    # nlohmann stores an integer literal outside [int64_min, uint64_max] as a double; we must match.
    out = namz.unpack(namz.pack(b'{"x":18446744073709551616,"weights":[1.0]}'))
    assert b'"x":1.8446744073709552e+19' in out, out
    # the exact boundaries stay integers
    out2 = namz.unpack(namz.pack(b'{"a":18446744073709551615,"b":-9223372036854775808,"weights":[1.0]}'))
    assert b'"a":18446744073709551615' in out2
    assert b'"b":-9223372036854775808' in out2


# -- CLI -------------------------------------------------------------------------------------------
def test_cli_encode_decode_roundtrip(tmp_path, capsys):
    nam = tmp_path / "in.nam"
    namz_out = tmp_path / "out.namz"
    back = tmp_path / "back.nam"
    nam.write_bytes(b'{"architecture":"WaveNet","weights":[0.5,-0.25,0.125]}')

    assert cli.main(["namz", "encode", str(nam), str(namz_out)]) == 0
    assert namz.is_namz(namz_out.read_bytes())
    assert cli.main(["namz", "decode", str(namz_out), str(back)]) == 0
    assert back.read_bytes() == b'{"architecture":"WaveNet","weights":[0.5,-0.25,0.125]}'


def test_cli_encode_no_shuffle_flag(tmp_path):
    nam = tmp_path / "in.nam"
    a = tmp_path / "a.namz"
    b = tmp_path / "b.namz"
    nam.write_bytes(b'{"architecture":"X","weights":[0.5,-0.5,0.25]}')
    cli.main(["namz", "encode", str(nam), str(a)])
    cli.main(["namz", "encode", str(nam), str(b), "--no-shuffle"])
    # shuffle flag byte differs; both decode to the same thing
    assert a.read_bytes()[7] == 1 and b.read_bytes()[7] == 0
    assert namz.unpack(a.read_bytes()) == namz.unpack(b.read_bytes())


def test_cli_set_metadata_and_map(tmp_path, capsys):
    nam = tmp_path / "in.nam"
    out = tmp_path / "out.namz"
    nam.write_bytes(b'{"architecture":"X","weights":[1.0]}')
    cli.main(["namz", "encode", str(nam), str(out), "--set", "tone_type=clean", "--set", "boost=true"])
    capsys.readouterr()
    assert cli.main(["namz", "map", str(out), "--json"]) == 0
    stdout = capsys.readouterr().out.strip()
    assert json.loads(stdout) == {"boost": "true", "tone_type": "clean"}


def test_cli_verify_ok(tmp_path):
    nam = tmp_path / "in.nam"
    nam.write_bytes(b'{"architecture":"X","weights":[0.1,0.2,0.3]}')
    assert cli.main(["namz", "verify", str(nam)]) == 0


def test_cli_bad_set_and_usage(tmp_path):
    nam = tmp_path / "in.nam"
    out = tmp_path / "out.namz"
    nam.write_bytes(b'{"architecture":"X","weights":[1.0]}')
    assert cli.main(["namz", "encode", str(nam), str(out), "--set", "noequals"]) == 2
    assert cli.main(["namz"]) == 2
    assert cli.main(["namz", "bogus-command"]) == 2


def test_cli_decode_rejects_corrupt(tmp_path, capsys):
    bad = tmp_path / "bad.namz"
    out = tmp_path / "out.nam"
    bad.write_bytes(b"NAMZ\x02\x00\x00\x00\x00\x00garbage")
    assert cli.main(["namz", "decode", str(bad), str(out)]) == 1
    assert not out.exists()
