# SPDX-License-Identifier: MIT
"""Command-line packer/unpacker: ``namz`` — mirrors the C++ reference CLI (cli/namz.cpp).

Usage::

    namz encode <in.nam> <out.namz> [--no-shuffle] [--set key=value ...]
    namz encode <in.nam> <out.nam16z> --f16 [--no-shuffle] [--set key=value ...]   float16, LOSSY
    namz decode <in.namz|in.nam16z> <out.nam>
    namz map    <in.namz|in.nam16z> [--json]   print the metadata header (no weight decode)
    namz verify <in.nam> [--f16]       pack->unpack round-trip check + ratio
"""

from __future__ import annotations

import sys
from typing import Any, List, Optional

from . import _json
from ._codec import DEFAULT_MAX_JSON_BYTES, DTYPE_F16, PackOptions, is_namz, pack, read_meta, unpack

_USAGE = """namz — lossless .nam <-> .namz codec

Usage:
  namz encode <in.nam> <out.namz> [--no-shuffle] [--set key=value ...]
  namz encode <in.nam> <out.nam16z> --f16 [--no-shuffle] [--set key=value ...]
                                        float16 weights: half the size, LOSSY
  namz decode <in.namz|in.nam16z> <out.nam>
  namz map    <in.namz|in.nam16z> [--json]   print the metadata header (no weight decode)
  namz verify <in.nam> [--f16]          pack->unpack round-trip check + ratio
"""


def _usage() -> int:
    sys.stderr.write(_USAGE)
    return 2


def _read(path: str) -> Optional[bytes]:
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def _write(path: str, data: bytes) -> bool:
    try:
        with open(path, "wb") as f:
            f.write(data)
        return True
    except OSError:
        return False


def _pct(numer: int, denom: int) -> float:
    return 0.0 if denom == 0 else 100.0 * numer / denom


def _named_f16(path: str) -> bool:
    """True if ``path`` ends in ``.nam16z``, ASCII case ignored — the name a float16 file goes by."""
    tail = path[-7:]
    return len(path) >= 7 and "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in tail) == ".nam16z"


def _weights_of(node: Any, out: List[List[float]]) -> List[List[float]]:
    """Every numeric ``"weights"`` array of a parsed ``.nam``, in pack's order (sorted keys, depth first)."""
    if isinstance(node, dict):
        for key in sorted(node.keys()):
            val = node[key]
            if key == "weights" and isinstance(val, list) and all(
                isinstance(x, (int, float)) and not isinstance(x, bool) for x in val
            ):
                out.append([float(x) for x in val])
            else:
                _weights_of(val, out)
    elif isinstance(node, list):
        for item in node:
            _weights_of(item, out)
    return out


def _do_encode(argv: List[str]) -> int:
    if len(argv) < 4:
        return _usage()
    src_path, out_path = argv[2], argv[3]
    opts = PackOptions()
    i = 4
    while i < len(argv):
        a = argv[i]
        if a == "--no-shuffle":
            opts.shuffle = False
        elif a == "--f16":
            opts.dtype = DTYPE_F16
        elif a == "--set" and i + 1 < len(argv):
            i += 1
            kv = argv[i]
            eq = kv.find("=")
            if eq < 0:
                sys.stderr.write("bad --set (need key=value): %s\n" % kv)
                return 2
            opts.metadata[kv[:eq]] = kv[eq + 1 :]
        else:
            sys.stderr.write("unknown option: %s\n" % a)
            return _usage()
        i += 1

    # float16 is lossy, so it is asked for by the flag, and the name says it: --f16 writes only a .nam16z, and
    # a .nam16z is only ever written with --f16. The name never picks the dtype by itself.
    f16 = opts.dtype == DTYPE_F16
    if f16 != _named_f16(out_path):
        if f16:
            sys.stderr.write("--f16 writes lossy float16 weights: name the output .nam16z, not %s\n" % out_path)
        else:
            sys.stderr.write("%s is named .nam16z: pass --f16 to write float16 weights (lossy)\n" % out_path)
        return 2
    src = _read(src_path)
    if src is None:
        sys.stderr.write("cannot read %s\n" % src_path)
        return 1
    packed = pack(src, opts)
    if not packed:
        if f16:
            sys.stderr.write("encode failed (not valid NAM JSON, or a weight past float16's range: "
                             "|w| >= 65520, rounds past 65504?): %s\n" % src_path)
        else:
            sys.stderr.write("encode failed (not valid NAM JSON?): %s\n" % src_path)
        return 1
    if not _write(out_path, packed):
        sys.stderr.write("cannot write %s\n" % out_path)
        return 1
    sys.stderr.write(
        "%s -> %s  (%d -> %d bytes, %.1f%%)\n"
        % (src_path, out_path, len(src), len(packed), _pct(len(packed), len(src)))
    )
    return 0


def _do_decode(argv: List[str]) -> int:
    if len(argv) < 4:
        return _usage()
    src_path, out_path = argv[2], argv[3]
    src = _read(src_path)
    if src is None:
        sys.stderr.write("cannot read %s\n" % src_path)
        return 1
    nam = unpack(src, DEFAULT_MAX_JSON_BYTES)
    if not nam:
        sys.stderr.write("decode failed (not a .namz / corrupt / over cap): %s\n" % src_path)
        return 1
    if not _write(out_path, nam):
        sys.stderr.write("cannot write %s\n" % out_path)
        return 1
    sys.stderr.write("%s -> %s  (%d -> %d bytes)\n" % (src_path, out_path, len(src), len(nam)))
    return 0


def _do_map(argv: List[str]) -> int:
    if len(argv) < 3:
        return _usage()
    src_path = argv[2]
    as_json = len(argv) >= 4 and argv[3] == "--json"
    src = _read(src_path)
    if src is None:
        sys.stderr.write("cannot read %s\n" % src_path)
        return 1
    if not is_namz(src):
        sys.stderr.write("not a .namz: %s\n" % src_path)
        return 1
    m = read_meta(src)
    if as_json:
        def esc(s: str) -> str:
            return s.replace("\\", "\\\\").replace('"', '\\"')

        parts = ['"%s":"%s"' % (esc(k), esc(v)) for k, v in sorted(m.items())]
        sys.stdout.write("{%s}\n" % ",".join(parts))
    else:
        for k in sorted(m):
            sys.stdout.write("  %s = %s\n" % (k, m[k]))
        if not m:
            sys.stderr.write("(no metadata header — v1 .namz or packed without --set)\n")
        if len(src) > 6 and src[6] == DTYPE_F16:
            sys.stderr.write("(float16 weights — lossy)\n")
    return 0


def _do_verify(argv: List[str]) -> int:
    if len(argv) < 3:
        return _usage()
    src_path = argv[2]
    opts = PackOptions()
    for a in argv[3:]:
        if a == "--f16":
            opts.dtype = DTYPE_F16
        else:
            sys.stderr.write("unknown option: %s\n" % a)
            return _usage()
    src = _read(src_path)
    if src is None:
        sys.stderr.write("cannot read %s\n" % src_path)
        return 1
    packed = pack(src, opts)
    if not packed:
        sys.stderr.write("FAIL pack: %s\n" % src_path)
        return 1
    back = unpack(packed, DEFAULT_MAX_JSON_BYTES)
    if not back:
        sys.stderr.write("FAIL unpack: %s\n" % src_path)
        return 1
    packed2 = pack(back, opts)
    idempotent = packed == packed2
    if opts.dtype == DTYPE_F16:
        # What float16 cost: the largest change of any weight against the float32 pack, in absolute terms.
        p32 = pack(src)
        u32 = unpack(p32, DEFAULT_MAX_JSON_BYTES) if p32 else b""
        w32 = _weights_of(_json.loads(_json.text_of(u32)), []) if u32 else None
        w16 = _weights_of(_json.loads(_json.text_of(back)), [])
        if w32 is None or len(w32) != len(w16):
            sys.stderr.write("FAIL decode: %s\n" % src_path)
            return 1
        worst = 0.0
        for s32, s16 in zip(w32, w16):
            for a32, a16 in zip(s32, s16):
                worst = max(worst, abs(a16 - a32))
        sys.stderr.write(
            "%s: %s  raw=%d namz=%d nam16z=%d (%.1f%%)  idempotent=%s  max|w16-w32|=%.3g\n"
            % (
                src_path,
                "OK" if idempotent else "MISMATCH",
                len(src),
                len(p32),
                len(packed),
                _pct(len(packed), len(src)),
                "yes" if idempotent else "no",
                worst,
            )
        )
        return 0 if idempotent else 1
    sys.stderr.write(
        "%s: %s  raw=%d namz=%d (%.1f%%)  idempotent=%s\n"
        % (
            src_path,
            "OK" if idempotent else "MISMATCH",
            len(src),
            len(packed),
            _pct(len(packed), len(src)),
            "yes" if idempotent else "no",
        )
    )
    return 0 if idempotent else 1


def main(argv: Optional[List[str]] = None) -> int:
    """Entry point. Returns a process exit code."""
    argv = list(sys.argv if argv is None else argv)
    if len(argv) < 2:
        return _usage()
    cmd = argv[1]
    if cmd == "encode":
        return _do_encode(argv)
    if cmd == "decode":
        return _do_decode(argv)
    if cmd == "map":
        return _do_map(argv)
    if cmd == "verify":
        return _do_verify(argv)
    return _usage()


if __name__ == "__main__":
    raise SystemExit(main())
