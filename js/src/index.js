// SPDX-License-Identifier: MIT
// namz — a tiny lossless codec for NeuralAmpModeler `.nam` files.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.
//
// Native JavaScript port of the C++ reference (`include/namz.h`), byte-for-byte compatible: pack()
// reproduces the reference `.namz` exactly and unpack() rehydrates weights bit-exact (float32). On request
// (`dtype: DTYPE_F16`) pack() stores each weight as an IEEE half instead — LOSSY, half the payload, a
// `.nam16z` by convention; unpack() reads either dtype by the file's dtype byte. Pure JS, zero dependencies. See NAMZ-FORMAT.md for the wire format and conformance/ for the shared vectors.

import { NamNum, formatFixed6, parse, serialize } from "./json.js";

export { MAX_DEPTH } from "./json.js";
export const FORMAT_VERSION = 2; // readers accept 1 (no meta block) and 2, nothing else
export const DEFAULT_MAX_JSON_BYTES = 256 * 1024 * 1024;

const MAGIC = Uint8Array.of(0x4e, 0x41, 0x4d, 0x5a); // 'N','A','M','Z'
const CODEC_STORE = 0;
/** dtype 0: each weight a float32 — lossless to the engine, and the default. */
export const DTYPE_F32 = 0;
/** dtype 1: each weight the IEEE-754 half nearest its float32 — LOSSY, written only when asked for. */
export const DTYPE_F16 = 1;
const FLAG_SHUFFLE = 1 << 0;
const INT64_MAX = 2n ** 63n - 1n;

const EMPTY = new Uint8Array(0);
const _enc = new TextEncoder();
const _dec = new TextDecoder("utf-8", { fatal: true });

// -- small byte helpers ----------------------------------------------------------------------------
function toBytes(data) {
  if (data instanceof Uint8Array) return data;
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (typeof data === "string") return _enc.encode(data);
  throw new TypeError("expected a Uint8Array, ArrayBuffer, or string");
}
function toText(data) {
  return typeof data === "string" ? data : _dec.decode(toBytes(data));
}
function u16le(v) {
  return Uint8Array.of(v & 0xff, (v >>> 8) & 0xff);
}
function u32le(v) {
  return Uint8Array.of(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff);
}
function concatBytes(parts) {
  let n = 0;
  for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}
function isPlainObject(x) {
  return typeof x === "object" && x !== null && !Array.isArray(x) && !(x instanceof NamNum);
}

// byte-plane shuffle: dst[plane*count + i] = src[4*i + plane] (reversible, lossless).
function shuffle(src, count) {
  const out = new Uint8Array(count * 4);
  for (let i = 0; i < count; i++) {
    out[i] = src[4 * i];
    out[count + i] = src[4 * i + 1];
    out[2 * count + i] = src[4 * i + 2];
    out[3 * count + i] = src[4 * i + 3];
  }
  return out;
}
function unshuffle(src, count) {
  const out = new Uint8Array(count * 4);
  for (let i = 0; i < count; i++) {
    out[4 * i] = src[i];
    out[4 * i + 1] = src[count + i];
    out[4 * i + 2] = src[2 * count + i];
    out[4 * i + 3] = src[3 * count + i];
  }
  return out;
}

// Bytes per stored weight: 4 for float32, 2 for float16, 0 for a dtype namz does not define.
function bytesPerWeight(dtype) {
  return dtype === DTYPE_F32 ? 4 : dtype === DTYPE_F16 ? 2 : 0;
}

// The IEEE-754 binary16 nearest a finite float32 (given as its bits), ties to even — on the bits, as the
// reference's halfOf, so no rounding of the platform can move it. -1 if its half overflows (|f| >= 65520,
// the tie above 65504 rounding to the even infinity) or it is not finite. A half subnormal is kept; below
// the smallest one's midpoint (2^-25, a tie that goes to even) is a zero of the float's sign.
function halfOf(x) {
  const sign = (x >>> 16) & 0x8000;
  const a = x & 0x7fffffff;
  if (a >= 0x7f800000) return -1; // NaN or infinity
  if (a >= 0x38800000) {
    // >= 2^-14: a normal half, or past half's range
    const e = (a >>> 23) - 112; // the half's biased exponent (127 - 15 = 112)
    if (e >= 31) return -1; // >= 2^16: past every half
    let h = (e << 10) | ((a >>> 13) & 0x3ff);
    const rest = a & 0x1fff; // the 13 mantissa bits half has no room for
    if (rest > 0x1000 || (rest === 0x1000 && (h & 1) !== 0)) h++; // a carry steps the exponent, as it should
    if (h >= 0x7c00) return -1; // rounded up to infinity
    return sign | h;
  }
  if (a < 0x33000000) return sign; // < 2^-25: nearer to zero than to the smallest subnormal
  // [2^-25, 2^-14): a half subnormal in units of 2^-24 — the float32 significand >> (126 - its exponent)
  const significand = (a & 0x7fffff) | 0x800000;
  const shift = 126 - (a >>> 23); // 14 .. 24
  let h = significand >>> shift;
  const rest = significand & ((1 << shift) - 1);
  const mid = 1 << (shift - 1);
  if (rest > mid || (rest === mid && (h & 1) !== 0)) h++; // 0x400 is the smallest normal half: also right
  return sign | h;
}

// The float32 bits of a half's value — exact, every half has one. A half infinity or NaN widens to the
// float32 infinity or NaN, which the reader's finite check then refuses.
function floatBitsOfHalf(h) {
  const sign = (h & 0x8000) << 16;
  const e = (h >>> 10) & 0x1f;
  let m = h & 0x3ff;
  if (e === 0x1f) return (sign | 0x7f800000 | (m << 13)) >>> 0;
  if (e !== 0) return (sign | ((e + 112) << 23) | (m << 13)) >>> 0;
  if (m === 0) return sign >>> 0;
  let exp = 113; // a subnormal m * 2^-24: normalise it
  while ((m & 0x400) === 0) {
    m <<= 1;
    exp--;
  }
  return (sign | (exp << 23) | ((m & 0x3ff) << 13)) >>> 0;
}

// The `total` halves of a float16 payload widened to float32. A half is little-endian, its low byte first;
// with `planes` the payload is every low byte, then every high byte.
function widenHalves(payload, total, planes) {
  const out = new Float32Array(total);
  const bits = new Uint32Array(out.buffer);
  for (let k = 0; k < total; k++) {
    const lo = planes ? payload[k] : payload[2 * k];
    const hi = planes ? payload[total + k] : payload[2 * k + 1];
    bits[k] = floatBitsOfHalf(lo | (hi << 8));
  }
  return out;
}

// Type a metadata string the way the reference typeValue does: "true"/"false" → bool, all-digits →
// int64 (else string), else string. A LEADING ZERO keeps it a string (NAMZ-FORMAT §metadata):
// "0012345" is an identifier and the zeros in front of it are part of what was stamped. The pattern
// below is JSON's own integer grammar, which forbids the redundant zero for the same reason.
function typeValue(s) {
  if (s === "true") return true;
  if (s === "false") return false;
  if (/^(0|[1-9][0-9]*)$/.test(s)) {
    // No leading zeros to strip any more, so the string's length IS the value's: 19 digits is where
    // int64 ends, and anything past it stays a string (matches std::stoll overflow).
    if (s.length <= 19) {
      const v = BigInt(s);
      if (v <= INT64_MAX) return NamNum.int(v);
    }
    return s;
  }
  return s;
}

function isNumericWeights(v) {
  return Array.isArray(v) && v.every((x) => x instanceof NamNum);
}

// DFS in sorted key order (== nlohmann std::map): extract every numeric "weights" array as float32 and
// replace it with its ordinal integer index. A "weights" member that already holds an integer literal
// throws: the skeleton would store it as a placeholder, which a reader fills with a segment or refuses.
function extractWeights(node, out) {
  if (isPlainObject(node)) {
    const keys = Object.keys(node).sort(_compareUtf8);
    for (const key of keys) {
      const val = node[key];
      if (key === "weights" && val instanceof NamNum && !val.isFloat) {
        throw new TypeError('an integer "weights" member would read back as a placeholder');
      }
      if (key === "weights" && isNumericWeights(val)) {
        out.push(Float32Array.from(val, (nn) => nn.toNumber()));
        node[key] = NamNum.int(BigInt(out.length - 1));
      } else {
        extractWeights(val, out);
      }
    }
  } else if (Array.isArray(node)) {
    for (const item of node) extractWeights(item, out);
  }
}

// DFS inverse: swap each integer "weights" placeholder for its float segment; false if an index is OOR.
function refillWeights(node, segs) {
  let ok = true;
  if (isPlainObject(node)) {
    for (const key of Object.keys(node)) {
      const val = node[key];
      if (key === "weights" && val instanceof NamNum && !val.isFloat) {
        const idx = val.int;
        if (idx >= 0n && idx < BigInt(segs.length)) {
          node[key] = Array.from(segs[Number(idx)], (v) => NamNum.float(v));
        } else {
          ok = false;
        }
      } else if (!refillWeights(val, segs)) {
        ok = false;
      }
    }
  } else if (Array.isArray(node)) {
    for (const item of node) if (!refillWeights(item, segs)) ok = false;
  }
  return ok;
}

const _compareUtf8 = (() => {
  const enc = new TextEncoder();
  return (a, b) => {
    if (a === b) return 0;
    const ba = enc.encode(a);
    const bb = enc.encode(b);
    const n = Math.min(ba.length, bb.length);
    for (let i = 0; i < n; i++) if (ba[i] !== bb[i]) return ba[i] - bb[i];
    return ba.length - bb.length;
  };
})();

// -- public API ------------------------------------------------------------------------------------
/** True if `data` begins with the `.namz` magic. Cheap; safe on any/short buffer. */
export function isNamz(data) {
  let b;
  try {
    b = data instanceof Uint8Array ? data : toBytes(data);
  } catch {
    return false;
  }
  return b.length >= 4 && b[0] === 0x4e && b[1] === 0x41 && b[2] === 0x4d && b[3] === 0x5a;
}

/**
 * Parse NAM JSON (`.nam`) bytes/string → packed `.namz` bytes.
 * @param {Uint8Array|ArrayBuffer|string} namJson
 * `options.dtype` is {@link DTYPE_F32} (the default, lossless) or {@link DTYPE_F16}: each weight's float32
 * rounded to the nearest IEEE half, ties to even — LOSSY, half the payload; name such a file `.nam16z`.
 * @param {{shuffle?: boolean, metadata?: Object<string,string>, dtype?: number}} [options]
 * @returns {Uint8Array} the `.namz`, or an EMPTY array on failure (invalid JSON, non-finite weight, nested
 *   deeper than {@link MAX_DEPTH}, a "weights" member holding an integer literal, a dtype other than
 *   DTYPE_F32 and DTYPE_F16, with DTYPE_F16 a weight past half's range: |w| >= 65520).
 */
export function pack(namJson, options = {}) {
  try {
    return _pack(namJson, options);
  } catch {
    return EMPTY; // mirrors the reference `catch (...) { return {}; }`
  }
}

function _pack(namJson, options) {
  const shuffleOn = options.shuffle !== false;
  const metadata = options.metadata || {};
  const dtype = options.dtype === undefined ? DTYPE_F32 : options.dtype;
  const tree = parse(toText(namJson));

  let headerBytes = EMPTY;
  const metaKeys = Object.keys(metadata);
  if (metaKeys.length) {
    // Metadata can only be stamped onto a JSON object; the reference throws (→ empty) otherwise.
    if (!isPlainObject(tree)) return EMPTY;
    if (!isPlainObject(tree.metadata)) tree.metadata = Object.create(null);
    const header = Object.create(null);
    for (const k of metaKeys) {
      const tv = typeValue(String(metadata[k]));
      tree.metadata[k] = tv;
      header[k] = tv;
    }
    headerBytes = _enc.encode(serialize(header));
    if (headerBytes.length > 0xffff) headerBytes = EMPTY; // u16 length; display metadata is tiny
  }

  const arrays = [];
  extractWeights(tree, arrays);
  // Non-finite weights (NaN/±Inf, or a decimal that overflows float32) are out of contract → reject.
  for (const a of arrays) {
    for (let i = 0; i < a.length; i++) if (!Number.isFinite(a[i])) return EMPTY;
  }
  const skeleton = _enc.encode(serialize(tree));

  if (bytesPerWeight(dtype) === 0) return EMPTY;

  let total = 0;
  for (const a of arrays) total += a.length;

  let halves = null; // DTYPE_F16 only: every weight's half, in payload order
  if (dtype === DTYPE_F16) {
    halves = new Uint16Array(total);
    let k = 0;
    for (const a of arrays) {
      const bits = new Uint32Array(a.buffer, a.byteOffset, a.length);
      for (let i = 0; i < bits.length; i++) {
        const h = halfOf(bits[i]);
        if (h < 0) return EMPTY;
        halves[k++] = h;
      }
    }
  }

  const parts = [u32le(skeleton.length), skeleton, u32le(arrays.length)];
  for (const a of arrays) parts.push(u32le(a.length));

  if (halves) {
    const d = new Uint8Array(2 * total);
    for (let i = 0; i < total; i++) {
      const lo = halves[i] & 0xff;
      const hi = halves[i] >>> 8;
      if (shuffleOn) {
        d[i] = lo;
        d[total + i] = hi;
      } else {
        d[2 * i] = lo;
        d[2 * i + 1] = hi;
      }
    }
    parts.push(d);
  } else if (shuffleOn && total > 0) {
    const flat = new Float32Array(total);
    let o = 0;
    for (const a of arrays) {
      flat.set(a, o);
      o += a.length;
    }
    parts.push(shuffle(new Uint8Array(flat.buffer, flat.byteOffset, flat.byteLength), total));
  } else {
    for (const a of arrays) {
      if (a.length) parts.push(new Uint8Array(a.buffer, a.byteOffset, a.byteLength));
    }
  }

  return concatBytes([
    MAGIC,
    Uint8Array.of(FORMAT_VERSION, CODEC_STORE, dtype, shuffleOn ? FLAG_SHUFFLE : 0),
    u16le(headerBytes.length),
    headerBytes,
    ...parts,
  ]);
}

/**
 * Inverse of {@link pack}: `.namz` bytes → reconstructed `.nam` JSON bytes. Reads either dtype by the file's
 * dtype byte, never its name: a float16 payload widens to the float32 of the same value, exactly.
 * Returns an EMPTY array on any failure (not a `.namz`, unknown codec/dtype, truncation, corruption,
 * over-cap). Never throws on malformed input.
 * @param {Uint8Array|ArrayBuffer|string} data
 * @param {number} [maxJsonBytes]
 * @returns {Uint8Array}
 */
export function unpack(data, maxJsonBytes = DEFAULT_MAX_JSON_BYTES) {
  let bytes;
  try {
    bytes = toBytes(data);
  } catch {
    return EMPTY; // total function: never throws, even on non-bytes input
  }
  const n = bytes.length;
  if (!isNamz(bytes) || n < 8) return EMPTY;

  const fmt = bytes[4];
  const codec = bytes[5];
  const dtype = bytes[6];
  const flags = bytes[7];
  const es = bytesPerWeight(dtype);
  if (fmt < 1 || fmt > FORMAT_VERSION || codec !== CODEC_STORE || es === 0) return EMPTY; // versions 1 and 2 only

  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let off = 8;
  if (fmt >= 2) {
    if (n < 10) return EMPTY;
    const metaLen = dv.getUint16(8, true);
    off = 10 + metaLen;
    if (off > n) return EMPTY;
  }

  if (n - off > maxJsonBytes + 4096) return EMPTY;

  try {
    let p = off;
    let rem = n - off;
    if (rem < 4) return EMPTY;
    const skeletonLen = dv.getUint32(p, true);
    p += 4;
    rem -= 4;
    if (skeletonLen > rem) return EMPTY;
    const skeleton = bytes.subarray(p, p + skeletonLen);
    p += skeletonLen;
    rem -= skeletonLen;

    if (rem < 4) return EMPTY;
    const numArrays = dv.getUint32(p, true);
    p += 4;
    rem -= 4;
    if (numArrays > Math.floor(rem / 4)) return EMPTY;

    const lengths = [];
    let total = 0;
    for (let i = 0; i < numArrays; i++) {
      const ln = dv.getUint32(p, true);
      p += 4;
      rem -= 4;
      lengths.push(ln);
      total += ln;
    }

    // The payload must be EXACTLY the rest of the buffer, `es` bytes a weight. Rejects truncation / lying
    // lengths.
    if (rem % es !== 0 || total !== rem / es) return EMPTY;

    const payload = bytes.subarray(p, p + rem);
    let f32;
    if (dtype === DTYPE_F16) {
      f32 = widenHalves(payload, total, (flags & FLAG_SHUFFLE) !== 0);
    } else {
      const raw = total > 0 && flags & FLAG_SHUFFLE ? unshuffle(payload, total) : payload;
      // Copy into a fresh, aligned buffer before viewing as float32 (payload may be an unaligned subarray).
      const aligned = new Uint8Array(total * 4);
      aligned.set(raw.subarray(0, total * 4));
      f32 = new Float32Array(aligned.buffer);
    }
    for (let i = 0; i < total; i++) if (!Number.isFinite(f32[i])) return EMPTY; // non-finite payload

    const segs = [];
    let o = 0;
    for (const ln of lengths) {
      segs.push(f32.subarray(o, o + ln));
      o += ln;
    }

    const tree = parse(_dec.decode(skeleton));
    if (!refillWeights(tree, segs)) return EMPTY;
    const rebuilt = _enc.encode(serialize(tree));
    if (rebuilt.length > maxJsonBytes) return EMPTY;
    return rebuilt;
  } catch {
    return EMPTY;
  }
}

/**
 * Read the display-metadata block WITHOUT touching the weights. Empty for a v1 `.namz`, a non-`.namz`
 * buffer, or one packed without metadata. Values are strings (bool → "true"/"false", integer → its digits,
 * other number → C's "%f", array or object → its JSON as the reference prints it).
 * @param {Uint8Array|ArrayBuffer|string} data
 * @returns {Object<string,string>}
 */
export function readMeta(data) {
  const out = {};
  let bytes;
  try {
    bytes = toBytes(data);
  } catch {
    return out;
  }
  if (!isNamz(bytes) || bytes.length < 10) return out;
  if (bytes[4] < 2) return out; // v1 has no meta block
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const metaLen = dv.getUint16(8, true);
  if (metaLen === 0 || 10 + metaLen > bytes.length) return out;
  let j;
  try {
    j = parse(_dec.decode(bytes.subarray(10, 10 + metaLen)));
  } catch {
    return out;
  }
  if (isPlainObject(j)) {
    for (const k of Object.keys(j)) {
      const v = j[k];
      if (typeof v === "string") out[k] = v;
      else if (v === true) out[k] = "true";
      else if (v === false) out[k] = "false";
      else if (v instanceof NamNum) out[k] = v.isFloat ? formatFixed6(v.num) : v.int.toString();
      else out[k] = serialize(v);
    }
  }
  return out;
}
