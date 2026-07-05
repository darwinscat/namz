# SPDX-License-Identifier: MIT
# namz — a tiny lossless codec for NeuralAmpModeler `.nam` files.
# Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.
#
# This is a native Python port of the C++ reference (`include/namz.h`). It is byte-for-byte compatible:
# `pack()` reproduces the reference `.namz` exactly, and `unpack()` rehydrates weights bit-exact (float32).
# See NAMZ-FORMAT.md for the wire format and conformance/ for the cross-language vectors this must satisfy.
"""Core codec: :func:`pack`, :func:`unpack`, :func:`read_meta`, :func:`is_namz`."""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Union

import numpy as np

__all__ = [
    "pack",
    "unpack",
    "read_meta",
    "is_namz",
    "PackOptions",
    "FORMAT_VERSION",
    "DEFAULT_MAX_JSON_BYTES",
]

# -- wire constants (mirror include/namz.h) --------------------------------------------------------
MAGIC = b"NAMZ"
FORMAT_VERSION = 2  # readers still accept 1 (no meta block)

_CODEC_STORE = 0  # 1 = deflate, 2 = zstd (reserved)
_DTYPE_F32 = 0  # 1 = float16 (reserved, lossy)
_FLAG_SHUFFLE = 1 << 0  # payload bytes split into 4 planes (lossless)

_INT64_MAX = (1 << 63) - 1
_INT64_MIN = -(1 << 63)
_UINT64_MAX = (1 << 64) - 1

#: Default cap on the reconstructed `.nam` JSON (zip-bomb guard), matching the reference CLI.
DEFAULT_MAX_JSON_BYTES = 256 * 1024 * 1024

# JSON serialization tuned to byte-match nlohmann's ``dump()``: minified, keys sorted ascending, raw
# UTF-8 (nlohmann does not escape non-ASCII by default). ``allow_nan=False`` makes serialization RAISE on
# any non-finite value rather than emit Python's non-standard ``NaN``/``Infinity`` tokens — non-finite is
# out of contract, so callers turn that raise into a clean rejection.
_JSON_KW = dict(sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False)


@dataclass
class PackOptions:
    """Options for :func:`pack`.

    Attributes
    ----------
    shuffle:
        Split each float's 4 bytes into 4 contiguous planes (lossless). Free, and helps whatever OUTER
        compressor later sees the file. Defaults to ``True`` (the reference default).
    metadata:
        Display fields to set/overwrite in the top-level ``metadata`` object and mirror into the readable
        header block. Values are typed on the way in: ``"true"``/``"false"`` → bool, all-ASCII-digits →
        int, else string — exactly like the reference. Empty ⇒ metadata untouched, no header block.
    """

    shuffle: bool = True
    metadata: Dict[str, str] = field(default_factory=dict)


# -- helpers ---------------------------------------------------------------------------------------
def _type_value(s: str) -> Union[bool, int, str]:
    """Type a metadata string the way the reference ``typeValue`` does."""
    if s == "true":
        return True
    if s == "false":
        return False
    if s and all(c in "0123456789" for c in s):
        # int64 max is 19 digits; anything longer can't fit → keep as string (matches std::stoll
        # overflow). The length gate also avoids int()'s 4300-digit conversion limit on Python ≥ 3.11.
        if len(s) <= 19:
            v = int(s)
            if v <= _INT64_MAX:
                return v
        return s
    return s


def _reject_nonfinite(_token: str) -> float:
    """Reject the ``NaN`` / ``Infinity`` / ``-Infinity`` JSON literals.

    Python's ``json`` accepts them by default; the C++ reference (nlohmann) does not — and non-finite
    weights are explicitly out of contract. Raising here makes :func:`pack` reject the same inputs the
    reference rejects (returns ``b""``).
    """
    raise ValueError("non-finite JSON literal")


def _is_numeric_weights(v: Any) -> bool:
    """True iff ``v`` is a JSON array of numbers (bools are NOT numbers, matching nlohmann)."""
    if not isinstance(v, list):
        return False
    for x in v:
        if isinstance(x, bool) or not isinstance(x, (int, float)):
            return False
    return True


def _extract_weights(node: Any, out: List[np.ndarray]) -> None:
    """DFS: pull every numeric ``"weights"`` array into ``out`` and replace it with its ordinal index.

    Objects are traversed in **sorted key order** to match nlohmann (``std::map``); this fixes both the
    ordinal indices and the concatenation order of the payload.
    """
    if isinstance(node, dict):
        for key in sorted(node.keys()):
            val = node[key]
            if key == "weights" and _is_numeric_weights(val):
                out.append(np.asarray(val, dtype=np.float32))
                node[key] = len(out) - 1
            else:
                _extract_weights(val, out)
    elif isinstance(node, list):
        for item in node:
            _extract_weights(item, out)


def _refill_weights(node: Any, segs: List[np.ndarray]) -> bool:
    """DFS inverse: swap each integer ``"weights"`` placeholder for its float segment.

    Returns ``False`` if any placeholder index is out of range — the signature of a corrupt stream.
    """
    ok = True
    if isinstance(node, dict):
        for key in list(node.keys()):
            val = node[key]
            if key == "weights" and isinstance(val, int) and not isinstance(val, bool):
                if 0 <= val < len(segs):
                    node[key] = segs[val].tolist()
                else:
                    ok = False
            elif not _refill_weights(val, segs):
                ok = False
    elif isinstance(node, list):
        for item in node:
            if not _refill_weights(item, segs):
                ok = False
    return ok


def _coerce_oversized_ints(node: Any) -> Any:
    """Match nlohmann: an integer literal outside ``[int64_min, uint64_max]`` is stored as a double.

    Applied only to the (weightless) skeleton tree, so it stays cheap. Returns the coerced node. This is
    byte-exact with the reference whenever the value is double-representable (e.g. ``2**64``); for extreme
    23+-digit magnitudes nlohmann's own number parser is not correctly rounded and may differ by one ULP,
    but such integers never appear in NAM config (which holds small dimensions and a few simple decimals).
    """
    if isinstance(node, bool):
        return node
    if isinstance(node, int):
        return node if _INT64_MIN <= node <= _UINT64_MAX else float(node)
    if isinstance(node, dict):
        for k in node:
            node[k] = _coerce_oversized_ints(node[k])
        return node
    if isinstance(node, list):
        for i in range(len(node)):
            node[i] = _coerce_oversized_ints(node[i])
        return node
    return node


def _shuffle(flat: np.ndarray) -> bytes:
    """AoS float32 bytes → 4 SoA byte-planes (reversible). ``flat`` is little-endian float32."""
    b = flat.view(np.uint8).reshape(-1, 4)  # (N, 4)
    return np.ascontiguousarray(b.T).tobytes()  # (4, N) row-major → plane0…plane3


def _unshuffle(payload: bytes, count: int) -> np.ndarray:
    """4 byte-planes → little-endian float32 array of ``count`` values."""
    planes = np.frombuffer(payload, dtype=np.uint8).reshape(4, count)  # (4, N)
    return np.ascontiguousarray(planes.T).view("<f4").reshape(-1)


# -- public API ------------------------------------------------------------------------------------
def is_namz(data: bytes) -> bool:
    """True if ``data`` begins with the ``.namz`` magic. Cheap; safe on any/short buffer."""
    return len(data) >= 4 and data[:4] == MAGIC


def pack(nam_json: Union[bytes, bytearray, str], options: Optional[PackOptions] = None) -> bytes:
    """Parse NAM JSON (``.nam``) bytes → packed ``.namz`` bytes.

    Returns ``b""`` on failure (not valid JSON). Lossless w.r.t. the float32 engine.
    """
    opts = options or PackOptions()
    try:
        return _pack(nam_json, opts)
    except (ValueError, TypeError, OverflowError):
        # Mirrors the reference `catch (...) { return {}; }`: any malformed input (bad JSON, a non-finite
        # or out-of-float32-range weight, an oversized integer) is a clean failure, never a raise.
        return b""


def _pack(nam_json: Union[bytes, bytearray, str], opts: PackOptions) -> bytes:
    j = json.loads(nam_json, parse_constant=_reject_nonfinite)

    header_bytes = b""
    if opts.metadata:
        # Metadata can only be stamped onto a JSON object. The reference does `j["metadata"] = …`, which
        # throws (→ empty) on a non-object skeleton (e.g. a top-level array); match that rejection.
        if not isinstance(j, dict):
            return b""
        if not isinstance(j.get("metadata"), dict):
            j["metadata"] = {}
        header: Dict[str, Any] = {}
        for k, v in opts.metadata.items():
            tv = _type_value(v)
            j["metadata"][k] = tv
            header[k] = tv
        header_bytes = json.dumps(header, **_JSON_KW).encode("utf-8")
        if len(header_bytes) > 0xFFFF:  # u16 length field; display metadata is tiny — never hit
            header_bytes = b""

    arrays: List[np.ndarray] = []
    _extract_weights(j, arrays)
    # Non-finite weights (NaN/±Inf, or a decimal that overflows float32) are out of contract; reject them
    # rather than emit a blob that can't round-trip through standard JSON.
    for a in arrays:
        if a.size and not np.isfinite(a).all():
            return b""
    j = _coerce_oversized_ints(j)  # weights are already extracted → only tiny config ints are walked
    skeleton = json.dumps(j, **_JSON_KW).encode("utf-8")

    total_floats = int(sum(a.size for a in arrays))

    body = bytearray()
    body += struct.pack("<I", len(skeleton))
    body += skeleton
    body += struct.pack("<I", len(arrays))
    for a in arrays:
        body += struct.pack("<I", a.size)

    if opts.shuffle and total_floats > 0:
        flat = np.concatenate([a.astype("<f4", copy=False) for a in arrays])
        body += _shuffle(flat)
    else:
        for a in arrays:
            if a.size:
                body += a.astype("<f4", copy=False).tobytes()

    out = bytearray()
    out += MAGIC
    out += bytes([FORMAT_VERSION, _CODEC_STORE, _DTYPE_F32, _FLAG_SHUFFLE if opts.shuffle else 0])
    out += struct.pack("<H", len(header_bytes))
    out += header_bytes
    out += body
    return bytes(out)


def unpack(data: Union[bytes, bytearray], max_json_bytes: int = DEFAULT_MAX_JSON_BYTES) -> bytes:
    """Inverse of :func:`pack`: ``.namz`` bytes → reconstructed ``.nam`` JSON bytes.

    Returns ``b""`` on any failure — not a ``.namz``, unknown codec/dtype, truncation, corruption, or an
    output over ``max_json_bytes``. Never raises on malformed input.
    """
    data = bytes(data)
    n = len(data)
    if not is_namz(data) or n < 8:
        return b""

    fmt, codec, dtype, flags = data[4], data[5], data[6], data[7]
    if fmt > FORMAT_VERSION or codec != _CODEC_STORE or dtype != _DTYPE_F32:
        return b""

    off = 8
    if fmt >= 2:
        if n < 10:
            return b""
        meta_len = struct.unpack_from("<H", data, 8)[0]
        off = 10 + meta_len
        if off > n:
            return b""

    # Cap the raw body against the reconstruction cap so a crafted blob can't force a huge read.
    if n - off > max_json_bytes + 4096:
        return b""

    try:
        p, rem = off, n - off
        if rem < 4:
            return b""
        skeleton_len = struct.unpack_from("<I", data, p)[0]
        p += 4
        rem -= 4
        if skeleton_len > rem:
            return b""
        skeleton = data[p : p + skeleton_len]
        p += skeleton_len
        rem -= skeleton_len

        if rem < 4:
            return b""
        num_arrays = struct.unpack_from("<I", data, p)[0]
        p += 4
        rem -= 4
        if num_arrays > rem // 4:  # can't hold that many u32 lengths → truncated / OOM guard
            return b""

        lengths: List[int] = []
        total_floats = 0
        for _ in range(num_arrays):
            ln = struct.unpack_from("<I", data, p)[0]
            p += 4
            rem -= 4
            lengths.append(ln)
            total_floats += ln

        # The float32 payload must be EXACTLY the rest of the buffer (division, not multiply → no
        # overflow). Rejects truncation and lying/short lengths.
        if rem % 4 != 0 or total_floats != rem // 4:
            return b""

        payload = data[p : p + rem]
        if total_floats > 0:
            if flags & _FLAG_SHUFFLE:
                flat = _unshuffle(payload, total_floats)
            else:
                flat = np.frombuffer(payload, dtype="<f4")
        else:
            flat = np.empty(0, dtype="<f4")

        segs: List[np.ndarray] = []
        o = 0
        for ln in lengths:
            segs.append(flat[o : o + ln])
            o += ln

        j = json.loads(skeleton)
        j = _coerce_oversized_ints(j)  # coerce BEFORE refill, while the tree is still weight-free
        if not _refill_weights(j, segs):
            return b""
        rebuilt = json.dumps(j, **_JSON_KW).encode("utf-8")
        if len(rebuilt) > max_json_bytes:
            return b""
        return rebuilt
    except (ValueError, struct.error, IndexError):
        return b""


def read_meta(data: bytes) -> Dict[str, str]:
    """Read the display-metadata block WITHOUT touching the weights.

    Empty for a v1 ``.namz``, a non-``.namz`` buffer, or one packed without metadata. Values are strings
    (bool → ``"true"``/``"false"``, number → its digits), mirroring the reference ``readMeta``.
    """
    data = bytes(data)
    out: Dict[str, str] = {}
    if not is_namz(data) or len(data) < 10:
        return out
    if data[4] < 2:  # v1 has no meta block
        return out
    meta_len = struct.unpack_from("<H", data, 8)[0]
    if meta_len == 0 or 10 + meta_len > len(data):
        return out
    try:
        j = json.loads(data[10 : 10 + meta_len])
    except ValueError:
        return out
    if isinstance(j, dict):
        for k, v in j.items():
            if isinstance(v, str):
                out[k] = v
            elif isinstance(v, bool):
                out[k] = "true" if v else "false"
            elif isinstance(v, int):
                out[k] = str(v)
            elif isinstance(v, float):
                out[k] = "%f" % v  # matches the reference std::to_string(double) (6 decimals)
            else:
                out[k] = json.dumps(v, ensure_ascii=False)
    return out
