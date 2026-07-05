# SPDX-License-Identifier: MIT
"""Command-line packer/unpacker: ``namz`` — mirrors the C++ reference CLI (cli/namz.cpp).

Usage::

    namz encode <in.nam> <out.namz> [--no-shuffle] [--set key=value ...]
    namz decode <in.namz> <out.nam>
    namz map    <in.namz> [--json]     print the metadata header (no weight decode)
    namz verify <in.nam>               pack->unpack round-trip check + ratio
"""

from __future__ import annotations

import sys
from typing import List, Optional

from ._codec import DEFAULT_MAX_JSON_BYTES, PackOptions, is_namz, pack, read_meta, unpack

_USAGE = """namz — lossless .nam <-> .namz codec

Usage:
  namz encode <in.nam> <out.namz> [--no-shuffle] [--set key=value ...]
  namz decode <in.namz> <out.nam>
  namz map    <in.namz> [--json]        print the metadata header (no weight decode)
  namz verify <in.nam>                  pack->unpack round-trip check + ratio
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

    src = _read(src_path)
    if src is None:
        sys.stderr.write("cannot read %s\n" % src_path)
        return 1
    packed = pack(src, opts)
    if not packed:
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
    return 0


def _do_verify(argv: List[str]) -> int:
    if len(argv) < 3:
        return _usage()
    src_path = argv[2]
    src = _read(src_path)
    if src is None:
        sys.stderr.write("cannot read %s\n" % src_path)
        return 1
    packed = pack(src)
    if not packed:
        sys.stderr.write("FAIL pack: %s\n" % src_path)
        return 1
    back = unpack(packed, DEFAULT_MAX_JSON_BYTES)
    if not back:
        sys.stderr.write("FAIL unpack: %s\n" % src_path)
        return 1
    packed2 = pack(back)
    idempotent = packed == packed2
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
