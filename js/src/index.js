// SPDX-License-Identifier: MIT
// namz — a tiny lossless codec for NeuralAmpModeler `.nam` files.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.
//
// Native JavaScript port of the C++ reference (`include/namz.h`), byte-for-byte compatible: pack()
// reproduces the reference `.namz` exactly and unpack() rehydrates weights bit-exact (float32). Pure JS,
// zero dependencies. See NAMZ-FORMAT.md for the wire format and conformance/ for the shared vectors.

import { NamNum, parse, serialize } from "./json.js";

export const FORMAT_VERSION = 2; // readers still accept 1 (no meta block)
export const DEFAULT_MAX_JSON_BYTES = 256 * 1024 * 1024;

const MAGIC = Uint8Array.of(0x4e, 0x41, 0x4d, 0x5a); // 'N','A','M','Z'
const CODEC_STORE = 0;
const DTYPE_F32 = 0;
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

// Type a metadata string the way the reference typeValue does: "true"/"false" → bool, all-digits →
// int64 (else string), else string.
function typeValue(s) {
  if (s === "true") return true;
  if (s === "false") return false;
  if (s.length > 0 && /^[0-9]+$/.test(s)) {
    // Match std::stoll: it ignores leading zeros and types by VALUE, not string length. Strip zeros
    // first (so "000…001" → 1), then only genuinely-too-big values stay strings.
    const stripped = s.replace(/^0+/, "");
    if (stripped === "") return NamNum.int(0n); // all zeros → 0
    if (stripped.length <= 19) {
      const v = BigInt(stripped);
      if (v <= INT64_MAX) return NamNum.int(v);
    }
    return s; // too big for int64 → keep as string (matches std::stoll overflow)
  }
  return s;
}

function isNumericWeights(v) {
  return Array.isArray(v) && v.every((x) => x instanceof NamNum);
}

// DFS in sorted key order (== nlohmann std::map): extract every numeric "weights" array as float32 and
// replace it with its ordinal integer index.
function extractWeights(node, out) {
  if (isPlainObject(node)) {
    const keys = Object.keys(node).sort(_compareUtf8);
    for (const key of keys) {
      const val = node[key];
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
 * @param {{shuffle?: boolean, metadata?: Object<string,string>}} [options]
 * @returns {Uint8Array} the `.namz`, or an EMPTY array on failure (invalid JSON, non-finite weight).
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

  let total = 0;
  for (const a of arrays) total += a.length;

  const parts = [u32le(skeleton.length), skeleton, u32le(arrays.length)];
  for (const a of arrays) parts.push(u32le(a.length));

  if (shuffleOn && total > 0) {
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
    Uint8Array.of(FORMAT_VERSION, CODEC_STORE, DTYPE_F32, shuffleOn ? FLAG_SHUFFLE : 0),
    u16le(headerBytes.length),
    headerBytes,
    ...parts,
  ]);
}

/**
 * Inverse of {@link pack}: `.namz` bytes → reconstructed `.nam` JSON bytes.
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
  if (fmt > FORMAT_VERSION || codec !== CODEC_STORE || dtype !== DTYPE_F32) return EMPTY;

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

    // The float32 payload must be EXACTLY the rest of the buffer. Rejects truncation / lying lengths.
    if (rem % 4 !== 0 || total !== rem / 4) return EMPTY;

    const payload = bytes.subarray(p, p + rem);
    const raw = total > 0 && flags & FLAG_SHUFFLE ? unshuffle(payload, total) : payload;
    // Copy into a fresh, aligned buffer before viewing as float32 (payload may be an unaligned subarray).
    const aligned = new Uint8Array(total * 4);
    aligned.set(raw.subarray(0, total * 4));
    const f32 = new Float32Array(aligned.buffer);
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
 * buffer, or one packed without metadata. Values are strings (bool → "true"/"false", number → digits).
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
      else if (v instanceof NamNum) out[k] = v.isFloat ? v.num.toFixed(6) : v.int.toString();
      else out[k] = serialize(v);
    }
  }
  return out;
}
