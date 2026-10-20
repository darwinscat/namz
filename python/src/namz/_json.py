# SPDX-License-Identifier: MIT
# namz — a tiny lossless codec for NeuralAmpModeler `.nam` files.
# Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.
"""JSON text read and written exactly as the C++ reference (nlohmann/json 3.11) reads and writes it.

Why not ``json.loads`` / ``json.dumps`` alone? Reading, nlohmann refuses what Python's ``json`` lets through: a
number past every double, an escaped lone surrogate, text deeper than :data:`MAX_DEPTH`, text in UTF-16 or UTF-32.
Writing, nlohmann picks a double's digits with Grisu2, which is neither always the shortest nor always the closest
(``-198.70986938476562`` prints as ``-198.70986938476563``), and switches to exponent form past 15 integer digits
(``1e+15``, where Python writes ``1000000000000000.0``). Python's ``json.dumps`` hard-codes ``float.__repr__``, so
:func:`dumps` is a serializer of its own around :func:`format_double`, a port of nlohmann's ``dtoa_impl``.
"""

from __future__ import annotations

import json
import math
import re
import struct
from json.encoder import encode_basestring  # escapes " \ and the C0 controls, non-ASCII raw: nlohmann's rule
from typing import Any, Callable, List, Union

import numpy as np

#: The deepest any JSON in a ``.namz`` may nest (NAMZ-FORMAT.md "Placeholders and depth", the reference's
#: ``namz::kMaxDepth``): a scalar is 0, an array or object is 1 + the deepest of its members, so an empty one
#: is 1 — counted as written, so a member a later duplicate key replaces still counts.
MAX_DEPTH = 128

_INT64_MIN = -(1 << 63)
_UINT64_MAX = (1 << 64) - 1

# A JSON string token or a bracket. The closing quote is optional so that an unterminated string ends the
# scan in one pass instead of being retried from every quote after it (that retry is quadratic).
_DEPTH_TOKENS = re.compile(r'"(?:[^"\\]|\\.)*"?|[\[\]{}]', re.DOTALL)
_STRING_TOKENS = re.compile(r'"(?:[^"\\]|\\.)*"?', re.DOTALL)


# -- reading ---------------------------------------------------------------------------------------
def text_of(data: Union[bytes, bytearray, memoryview]) -> str:
    """JSON bytes as text: UTF-8, one leading byte-order mark skipped, as nlohmann reads them. Anything else —
    UTF-16, UTF-32, a malformed sequence — raises ``ValueError`` (``json.loads`` would guess an encoding)."""
    return bytes(data).decode("utf-8-sig")


def depth_within(text: str, limit: int = MAX_DEPTH) -> bool:
    """True if the JSON ``text`` nests no deeper than ``limit`` — measured on the text, before it is parsed.

    ``json.loads`` recurses once per level, so a tree too deep to parse must be caught before parsing it:
    this scan counts the brackets outside strings, iteratively, and the parse that follows never descends
    past ``limit``. That is the reference's own count (as written, duplicates included); invalid JSON is
    refused by the parse anyway. A text with no more brackets than ``limit`` cannot nest deeper, which skips
    the scan for every real capture (its weights are one bracket pair, however many numbers they hold).
    """
    if text.count("[") + text.count("{") <= limit:
        return True
    depth = 0
    for m in _DEPTH_TOKENS.finditer(text):
        t = m.group()
        if t == "[" or t == "{":
            depth += 1
            if depth > limit:
                return False
        elif t[:1] != '"':  # a closing bracket
            depth -= 1
    return True


def _strings_well_formed(text: str) -> bool:
    """False if a string token escapes a lone surrogate (``"\\ud800"``): no Unicode text, refused by nlohmann
    wherever it stands — a value a later duplicate key replaces included. Free unless the text escapes a
    ``\\u`` at all."""
    if "\\u" not in text:
        return True
    for m in _STRING_TOKENS.finditer(text):
        tok = m.group()
        if "\\u" in tok:
            try:
                json.loads(tok).encode("utf-8")
            except ValueError:  # UnicodeEncodeError is one
                return False
    return True


def _parse_int(token: str) -> Union[int, float]:
    """An integer literal as nlohmann holds it: an integer within int64 or uint64, else a double — and past
    every double, no number at all (nlohmann refuses the text)."""
    if len(token) <= 20:  # "-9223372036854775808", "18446744073709551615"
        v = int(token)
        if _INT64_MIN <= v <= _UINT64_MAX:
            return v
    return _parse_float(token)


def _parse_float(token: str) -> float:
    v = float(token)
    if math.isinf(v):
        raise ValueError("number overflow: " + token[:40])
    return v


def _reject_constant(token: str) -> float:
    """``NaN`` / ``Infinity`` / ``-Infinity`` are not JSON; Python's ``json`` reads them, nlohmann does not."""
    raise ValueError("not a JSON literal: " + token)


def loads(text: str) -> Any:
    """Parse JSON ``text`` exactly where nlohmann parses it, else raise ``ValueError``."""
    if not depth_within(text):
        raise ValueError("nested deeper than %d" % MAX_DEPTH)
    if not _strings_well_formed(text):
        raise ValueError("a lone surrogate escape")
    return json.loads(text, parse_int=_parse_int, parse_float=_parse_float, parse_constant=_reject_constant)


# -- writing ---------------------------------------------------------------------------------------
# nlohmann's dtoa_impl (to_chars.hpp, after Florian Loitsch's Grisu2), step for step: Python's integers stand in
# for its uint64_t, and no step of it ever needs more than 64 bits, so nothing here wraps where it would not.
#
# Cached powers of ten c = f * 2^e ~= 10^k, k = -300 + 8 i: nlohmann's table, verbatim (f, e).
_CACHED_POWERS = (
    (0xAB70FE17C79AC6CA, -1060), (0xFF77B1FCBEBCDC4F, -1034), (0xBE5691EF416BD60C, -1007), (0x8DD01FAD907FFC3C, -980),
    (0xD3515C2831559A83, -954), (0x9D71AC8FADA6C9B5, -927), (0xEA9C227723EE8BCB, -901), (0xAECC49914078536D, -874),
    (0x823C12795DB6CE57, -847), (0xC21094364DFB5637, -821), (0x9096EA6F3848984F, -794), (0xD77485CB25823AC7, -768),
    (0xA086CFCD97BF97F4, -741), (0xEF340A98172AACE5, -715), (0xB23867FB2A35B28E, -688), (0x84C8D4DFD2C63F3B, -661),
    (0xC5DD44271AD3CDBA, -635), (0x936B9FCEBB25C996, -608), (0xDBAC6C247D62A584, -582), (0xA3AB66580D5FDAF6, -555),
    (0xF3E2F893DEC3F126, -529), (0xB5B5ADA8AAFF80B8, -502), (0x87625F056C7C4A8B, -475), (0xC9BCFF6034C13053, -449),
    (0x964E858C91BA2655, -422), (0xDFF9772470297EBD, -396), (0xA6DFBD9FB8E5B88F, -369), (0xF8A95FCF88747D94, -343),
    (0xB94470938FA89BCF, -316), (0x8A08F0F8BF0F156B, -289), (0xCDB02555653131B6, -263), (0x993FE2C6D07B7FAC, -236),
    (0xE45C10C42A2B3B06, -210), (0xAA242499697392D3, -183), (0xFD87B5F28300CA0E, -157), (0xBCE5086492111AEB, -130),
    (0x8CBCCC096F5088CC, -103), (0xD1B71758E219652C, -77), (0x9C40000000000000, -50), (0xE8D4A51000000000, -24),
    (0xAD78EBC5AC620000, 3), (0x813F3978F8940984, 30), (0xC097CE7BC90715B3, 56), (0x8F7E32CE7BEA5C70, 83),
    (0xD5D238A4ABE98068, 109), (0x9F4F2726179A2245, 136), (0xED63A231D4C4FB27, 162), (0xB0DE65388CC8ADA8, 189),
    (0x83C7088E1AAB65DB, 216), (0xC45D1DF942711D9A, 242), (0x924D692CA61BE758, 269), (0xDA01EE641A708DEA, 295),
    (0xA26DA3999AEF774A, 322), (0xF209787BB47D6B85, 348), (0xB454E4A179DD1877, 375), (0x865B86925B9BC5C2, 402),
    (0xC83553C5C8965D3D, 428), (0x952AB45CFA97A0B3, 455), (0xDE469FBD99A05FE3, 481), (0xA59BC234DB398C25, 508),
    (0xF6C69A72A3989F5C, 534), (0xB7DCBF5354E9BECE, 561), (0x88FCF317F22241E2, 588), (0xCC20CE9BD35C78A5, 614),
    (0x98165AF37B2153DF, 641), (0xE2A0B5DC971F303A, 667), (0xA8D9D1535CE3B396, 694), (0xFB9B7CD9A4A7443C, 720),
    (0xBB764C4CA7A44410, 747), (0x8BAB8EEFB6409C1A, 774), (0xD01FEF10A657842C, 800), (0x9B10A4E5E9913129, 827),
    (0xE7109BFBA19C0C9D, 853), (0xAC2820D9623BF429, 880), (0x80444B5E7AA7CF85, 907), (0xBF21E44003ACDD2D, 933),
    (0x8E679C2F5E44FF8F, 960), (0xD433179D9C8CB841, 986), (0x9E19DB92B4E31BA9, 1013),
)

_DOUBLE_BITS = struct.Struct("<d")
_U64 = struct.Struct("<Q")
_HALF = 1 << 63


def _grisu2(bits: int) -> "tuple[str, int]":
    """Digits and decimal exponent of the finite double ``value > 0`` whose bits are ``bits``:
    ``value ~= int(digits) * 10**exponent``."""
    big_e = bits >> 52
    frac = bits & 0xFFFFFFFFFFFFF
    if big_e:
        vf, ve = frac | 0x10000000000000, big_e - 1075
    else:
        vf, ve = frac, -1074
    # compute_boundaries: m+ and m- halfway to the neighbours, normalized to a 64-bit significand; v, m- and m+
    # all land on m+'s exponent
    mpf = 2 * vf + 1
    if frac == 0 and big_e > 1:  # the lower neighbour is closer
        mmf, mme = 4 * vf - 1, ve - 2
    else:
        mmf, mme = 2 * vf - 1, ve - 1
    shift = 64 - mpf.bit_length()
    mpf <<= shift
    e = ve - 1 - shift
    mmf <<= mme - e
    vf <<= ve - e
    # get_cached_power_for_binary_exponent: C++ integer division truncates toward zero
    f = -61 - e  # kAlpha - e - 1
    t = f * 78913
    k = (t >> 18) if t >= 0 else -((-t) >> 18)
    if f > 0:
        k += 1
    index = (307 + k) // 8
    cf, ce = _CACHED_POWERS[index]
    # grisu2: diyfp::mul keeps the upper 64 bits, rounded half up
    w = (vf * cf + _HALF) >> 64
    lo = ((mmf * cf + _HALF) >> 64) + 1
    hi = ((mpf * cf + _HALF) >> 64) - 1
    exponent = 300 - 8 * index  # -cached.k
    neg_e = -(e + ce + 64)
    mask = (1 << neg_e) - 1
    # grisu2_digit_gen; the digits so far are the integer `acc` (the first one is never 0), so a decrement of
    # the last digit (never below 0 in nlohmann) is a decrement of `acc`
    delta = hi - lo
    dist = hi - w
    p1 = hi >> neg_e
    p2 = hi & mask
    acc = 0
    n = len(str(p1)) if p1 else 1  # find_largest_pow10
    pow10 = 10 ** (n - 1)
    while n > 0:
        d, p1 = divmod(p1, pow10)
        acc = acc * 10 + d
        n -= 1
        rest = (p1 << neg_e) + p2
        if rest <= delta:
            ten_k = pow10 << neg_e
            while rest < dist and delta - rest >= ten_k and (rest + ten_k < dist or dist - rest > rest + ten_k - dist):
                acc -= 1  # grisu2_round
                rest += ten_k
            return str(acc), exponent + n
        pow10 //= 10
    m = 0
    while True:
        p2 *= 10
        acc = acc * 10 + (p2 >> neg_e)
        p2 &= mask
        m += 1
        delta *= 10
        dist *= 10
        if p2 <= delta:
            break
    ten_k = 1 << neg_e
    while p2 < dist and delta - p2 >= ten_k and (p2 + ten_k < dist or dist - p2 > p2 + ten_k - dist):
        acc -= 1  # grisu2_round
        p2 += ten_k
    return str(acc), exponent - m


def format_double(x: float) -> str:
    """A double exactly as nlohmann's ``dump()`` prints it: Grisu2's digits, fixed form while the decimal
    point sits within 15 digits of the first one (``-4 < n <= 15``), else ``d.ddde±XX``. Non-finite raises
    ``ValueError`` (nlohmann prints ``null``, which no reader takes back as a number)."""
    if x != x or x in (math.inf, -math.inf):
        raise ValueError("non-finite number")
    return _format_bits(_U64.unpack(_DOUBLE_BITS.pack(x))[0])


def _format_bits(bits: int) -> str:
    """:func:`format_double` of the finite double whose bits are ``bits``."""
    sign = ""
    if bits >> 63:
        sign, bits = "-", bits & 0x7FFFFFFFFFFFFFFF
    if bits == 0:
        return sign + "0.0"
    s, exponent = _grisu2(bits)
    k = len(s)
    n = k + exponent  # digits before the decimal point
    if k <= n <= 15:
        return sign + s + "0" * (n - k) + ".0"
    if 0 < n <= 15:
        return sign + s[:n] + "." + s[n:]
    if -4 < n <= 0:
        return sign + "0." + "0" * (-n) + s
    e = n - 1
    return sign + (s if k == 1 else s[0] + "." + s[1:]) + ("e-" if e < 0 else "e+") + "%02d" % abs(e)


def dumps(node: Any) -> str:
    """``node`` as nlohmann's ``dump()`` writes it: minified, keys in UTF-8 byte order, non-ASCII raw, doubles
    by :func:`format_double`. A float32 segment may stand in the tree as a numpy array. Recursion stops at
    the depth :func:`loads` admits."""
    out: List[str] = []
    _write(node, out.append)
    return "".join(out)


def _write(node: Any, put: Callable[[str], Any]) -> None:
    if node is None:
        put("null")
    elif node is True:
        put("true")
    elif node is False:
        put("false")
    elif isinstance(node, str):
        put(encode_basestring(node))
    elif isinstance(node, int):
        put(str(node))
    elif isinstance(node, float):
        put(format_double(node))
    elif isinstance(node, dict):
        put("{")
        first = True
        for key in sorted(node):  # code-point order is UTF-8 byte order
            if not first:
                put(",")
            first = False
            put(encode_basestring(key))
            put(":")
            _write(node[key], put)
        put("}")
    elif isinstance(node, np.ndarray):  # a float32 segment: finite (unpack checked), each one's double's bits
        put("[" + ",".join(map(_format_bits, node.astype(np.float64).view(np.uint64).tolist())) + "]")
    elif isinstance(node, list):
        put("[")
        for i, item in enumerate(node):
            if i:
                put(",")
            _write(item, put)
        put("]")
    else:
        raise TypeError("not a JSON value: %r" % type(node))
