# SPDX-License-Identifier: MIT
"""The float16 dtype (``.nam16z``): pack writes it only when asked, each weight the half nearest its float32
(ties to even, subnormals kept), and unpack widens it back exactly. Checked against numpy.float16 — numpy's
own rounding, sharing no code with the port's bit arithmetic — on every finite half, every tie between two
halves and the float32 on either side of it.
"""

from __future__ import annotations

import json
import struct

import numpy as np
import pytest

import namz
from namz import cli

FINITE = np.array([h for h in range(0x10000) if (h >> 10) & 0x1F != 0x1F], dtype=np.uint16)


def _literal(v: float) -> str:
    return "-0.0" if v == 0 and np.signbit(v) else repr(float(v))  # "-0" is the integer 0, as in the reference


def _nam(values) -> bytes:
    return ('{"architecture":"X","weights":[%s]}' % ",".join(_literal(v) for v in values)).encode()


def _weights(text: bytes) -> np.ndarray:
    return np.asarray(json.loads(text)["weights"], dtype=np.float64).astype(np.float32)


def _halves(blob: bytes, n: int) -> np.ndarray:
    """The last 2n payload bytes of an unshuffled float16 file, as its n halves."""
    return np.frombuffer(blob[len(blob) - 2 * n:], dtype="<u2")


def test_every_finite_half_packs_to_itself_and_widens_back_exactly():
    values = FINITE.view(np.float16).astype(np.float32)
    blob = namz.pack(_nam(values), namz.PackOptions(dtype=namz.DTYPE_F16, shuffle=False))
    assert blob and blob[6] == namz.DTYPE_F16
    assert np.array_equal(_halves(blob, FINITE.size), FINITE)
    assert _weights(namz.unpack(blob)).view(np.uint32).tolist() == values.view(np.uint32).tolist()


def test_every_tie_and_its_float32_neighbours_round_as_numpy_does():
    h = np.arange(0, 0x7BFF, dtype=np.uint16)
    lo = h.view(np.float16).astype(np.float64)
    hi = (h + 1).astype(np.uint16).view(np.float16).astype(np.float64)
    mid = ((lo + hi) / 2).astype(np.float32)  # 12 significant bits: a float32 exactly
    bits = mid.view(np.uint32)
    near = np.concatenate([mid, (bits - 1).view(np.float32), (bits + 1).view(np.float32)])
    near = np.concatenate([near, -near, np.array([65519.996, -65519.996], dtype=np.float32)])
    want = near.astype(np.float16).astype(np.float32).view(np.uint32)
    for shuffle in (True, False):
        blob = namz.pack(_nam(near), namz.PackOptions(dtype=namz.DTYPE_F16, shuffle=shuffle))
        got = _weights(namz.unpack(blob)).view(np.uint32)
        assert np.array_equal(got, want), "shuffle %s: %d differ" % (shuffle, int((got != want).sum()))


@pytest.mark.parametrize("w", ["65520", "-65520", "65520.5", "100000", "3.4028234663852886e+38", "1e39"])
def test_pack_refuses_a_weight_past_halfs_range_and_the_whole_nam_with_it(w):
    nam = ('{"config":{"weights":[0.5]},"weights":[0.25,%s]}' % w).encode()
    assert namz.pack(nam, namz.PackOptions(dtype=namz.DTYPE_F16)) == b""


def test_the_top_of_the_range_rounds_to_65504():
    assert namz.pack(b'{"weights":[65520]}') != b"", "float32 takes 65520"
    top = namz.pack(b'{"weights":[65519.99609375,-65519]}', namz.PackOptions(dtype=namz.DTYPE_F16, shuffle=False))
    assert _halves(top, 2).tolist() == [0x7BFF, 0xFBFF]


def test_dtype_is_f32_by_default_and_nothing_but_0_or_1_is_a_dtype():
    nam = b'{"architecture":"X","weights":[0.1,0.2]}'
    plain = namz.pack(nam)
    assert plain[6] == namz.DTYPE_F32 == namz.PackOptions().dtype
    assert namz.pack(nam, namz.PackOptions(dtype=namz.DTYPE_F32)) == plain
    for d in (2, -1, 255, "1", True, 1.0, None, np.uint8(2), np.float32(1), np.bool_(True), np.int64(-1)):
        assert namz.pack(nam, namz.PackOptions(dtype=d)) == b"", repr(d)
    # numpy's own integers are integers: the same dtype as the Python int they equal
    f16 = namz.pack(nam, namz.PackOptions(dtype=namz.DTYPE_F16))
    for d in (np.int64(1), np.uint8(1), np.int32(1)):
        assert namz.pack(nam, namz.PackOptions(dtype=d)) == f16, repr(d)
    assert namz.pack(nam, namz.PackOptions(dtype=np.uint8(0))) == plain


def test_the_planes_are_every_low_byte_then_every_high_byte():
    nam = b'{"weights":[1.0,-2.0,0.5]}'  # 0x3c00 0xc000 0x3800
    a = namz.pack(nam, namz.PackOptions(dtype=namz.DTYPE_F16))
    b = namz.pack(nam, namz.PackOptions(dtype=namz.DTYPE_F16, shuffle=False))
    assert a[-6:] == bytes([0x00, 0x00, 0x00, 0x3C, 0xC0, 0x38]) and a[7] == 1
    assert b[-6:] == bytes([0x00, 0x3C, 0x00, 0xC0, 0x00, 0x38]) and b[7] == 0
    assert namz.unpack(a) == namz.unpack(b) == b'{"weights":[1.0,-2.0,0.5]}'


def test_skeleton_and_meta_are_the_float32_files_and_repacking_is_a_fixed_point():
    nam = b'{"architecture":"WaveNet","config":{"weights":[0.3,0.7]},"weights":[0.1,-0.2,0.333]}'
    m = {"tone_type": "clean"}
    p32 = namz.pack(nam, namz.PackOptions(metadata=dict(m)))
    p16 = namz.pack(nam, namz.PackOptions(metadata=dict(m), dtype=namz.DTYPE_F16))
    head = len(p32) - 5 * 4
    assert p16[:6] == p32[:6] and p16[7:head] == p32[7:head] and len(p16) == head + 5 * 2
    assert namz.pack(namz.unpack(p16), namz.PackOptions(metadata=dict(m), dtype=namz.DTYPE_F16)) == p16
    # a .nam packed as float16 and the .nam its float32 file unpacks to, packed as float16: the same file
    assert namz.pack(namz.unpack(p32), namz.PackOptions(metadata=dict(m), dtype=namz.DTYPE_F16)) == p16


def _file(halves, extra=b"", dtype=1):
    sk = b'{"weights":0}'
    return (b"NAMZ" + bytes([2, 0, dtype, 0, 0, 0]) + struct.pack("<I", len(sk)) + sk
            + struct.pack("<II", 1, len(halves)) + b"".join(struct.pack("<H", h) for h in halves) + extra)


def test_unpack_refuses_inf_nan_odd_payloads_and_dtype_2_up():
    assert namz.unpack(_file([0x3C00, 0x0001])) == b'{"weights":[1.0,5.960464477539063e-08]}'
    for bad in (0x7C00, 0xFC00, 0x7C01, 0x7E00, 0xFFFF):
        assert namz.unpack(_file([0x3C00, bad])) == b"", hex(bad)
    assert namz.unpack(_file([0x3C00], b"\x00")) == b"", "odd payload"
    assert namz.unpack(_file([0x3C00], b"\x00\x3c")) == b"", "one half too many"
    for d in range(2, 256):
        assert namz.unpack(_file([0x3C00], dtype=d)) == b"", "dtype %d" % d


def test_cli_f16_name_rule_map_and_verify(tmp_path, capsys):
    nam = tmp_path / "in.nam"
    nam.write_bytes(b'{"architecture":"X","weights":[0.1,0.2]}')
    assert cli.main(["namz", "encode", str(nam), str(tmp_path / "a.nam16z"), "--f16"]) == 0
    assert (tmp_path / "a.nam16z").read_bytes()[6] == namz.DTYPE_F16
    assert cli.main(["namz", "encode", str(nam), str(tmp_path / "b.NAM16Z"), "--f16", "--no-shuffle"]) == 0
    capsys.readouterr()
    assert cli.main(["namz", "encode", str(nam), str(tmp_path / "c.namz"), "--f16"]) == 2
    assert "name the output .nam16z" in capsys.readouterr().err
    assert cli.main(["namz", "encode", str(nam), str(tmp_path / "e.nam16z")]) == 2
    assert "pass --f16" in capsys.readouterr().err
    assert not (tmp_path / "c.namz").exists() and not (tmp_path / "e.nam16z").exists()
    big = tmp_path / "big.nam"
    big.write_bytes(b'{"weights":[65520]}')
    assert cli.main(["namz", "encode", str(big), str(tmp_path / "big.nam16z"), "--f16"]) == 1
    assert "65520" in capsys.readouterr().err
    assert cli.main(["namz", "map", str(tmp_path / "a.nam16z")]) == 0
    assert capsys.readouterr().err.endswith("(float16 weights — lossy)\n")
    assert cli.main(["namz", "verify", str(nam), "--f16"]) == 0
    # the reference prints, for these two weights: "in.nam: OK  raw=40 namz=62 nam16z=58 (145.0%)  idempotent=yes  max|w16-w32|=4.88e-05"
    want = "%s: OK  raw=40 namz=62 nam16z=58 (145.0%%)  idempotent=yes  max|w16-w32|=4.88e-05\n" % nam
    assert capsys.readouterr().err == want
    assert cli.main(["namz", "verify", str(nam), "--bogus"]) == 2
    assert "unknown option: --bogus" in capsys.readouterr().err
