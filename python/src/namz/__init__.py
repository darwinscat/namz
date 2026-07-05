# SPDX-License-Identifier: MIT
"""namz — lossless codec for NeuralAmpModeler ``.nam`` files.

A ``.nam`` is JSON whose bulk is one or more flat ``"weights"`` arrays written as ~20-char decimal
strings that the NAM engine loads as float32 anyway. ``.namz`` stores each weight as a 4-byte float32:
~5.5× smaller, bit-exact, deterministic — byte-identical to the C++ reference and every other port.

    >>> import namz
    >>> blob = namz.pack(b'{"architecture":"WaveNet","weights":[0.5,-0.5,0.25]}')
    >>> namz.is_namz(blob)
    True
    >>> namz.unpack(blob)
    b'{"architecture":"WaveNet","weights":[0.5,-0.5,0.25]}'
"""

from ._codec import (
    DEFAULT_MAX_JSON_BYTES,
    FORMAT_VERSION,
    PackOptions,
    is_namz,
    pack,
    read_meta,
    unpack,
)

__version__ = "1.0.0"

__all__ = [
    "pack",
    "unpack",
    "read_meta",
    "is_namz",
    "PackOptions",
    "FORMAT_VERSION",
    "DEFAULT_MAX_JSON_BYTES",
    "__version__",
]
