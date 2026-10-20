// SPDX-License-Identifier: MIT
// Falsification suite — actively try to BREAK the codec (mirrors tests/adversarial.cpp).
// Invariants: unpack NEVER throws; any non-empty output is a fixed point of pack∘unpack; a stored format
// rejects EVERY truncation and EVERY trailing byte; corrupt header/length/index bytes are rejected.
import assert from "node:assert/strict";
import test from "node:test";
import * as api from "../src/index.js";
import { DEFAULT_MAX_JSON_BYTES, isNamz, pack, readMeta, unpack } from "../src/index.js";
import { bytesEqual, readVector } from "./helpers.js";

const FULL = new Uint8Array(readVector("vectors/flat.namz")); // valid, shuffled, real payload

function mulberry32(seed) {
  return function () {
    seed |= 0;
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

test("sanity: flat.namz decodes", () => assert.ok(unpack(FULL).length));

test("unpack never throws on arbitrary garbage", () => {
  const rng = mulberry32(1234);
  for (let it = 0; it < 4000; it++) {
    const n = Math.floor(rng() * 64);
    const blob = new Uint8Array(n);
    for (let i = 0; i < n; i++) blob[i] = (rng() * 256) | 0;
    if (it % 2 === 0 && n >= 4) blob.set([0x4e, 0x41, 0x4d, 0x5a]); // half start with magic
    assert.ok(unpack(blob) instanceof Uint8Array);
  }
});

test("pack never throws on garbage", () => {
  const rng = mulberry32(99);
  for (let it = 0; it < 1000; it++) {
    const n = Math.floor(rng() * 64);
    const blob = new Uint8Array(n);
    for (let i = 0; i < n; i++) blob[i] = (rng() * 256) | 0;
    const out = pack(blob);
    assert.ok(out.length === 0 || isNamz(out));
  }
});

test("every truncation is rejected (stored format, exact-length)", () => {
  for (let L = 0; L < FULL.length; L++) {
    assert.equal(unpack(FULL.subarray(0, L)).length, 0, `prefix len ${L} accepted`);
  }
});

test("every trailing extension is rejected", () => {
  for (const k of [1, 2, 3, 4, 5, 7, 16, 64, 4096]) {
    const ext = new Uint8Array(FULL.length + k);
    ext.set(FULL);
    assert.equal(unpack(ext).length, 0, `+${k} trailing bytes accepted`);
  }
});

test("corrupting header-critical bytes rejects", () => {
  for (const pos of [0, 1, 2, 3, 4, 5, 6]) {
    const b = Uint8Array.from(FULL);
    b[pos] ^= 0xff;
    assert.equal(unpack(b).length, 0, `header byte ${pos} corruption accepted`);
  }
  const withByte = (pos, val) => {
    const b = Uint8Array.from(FULL);
    b[pos] = val;
    return b;
  };
  assert.equal(unpack(withByte(4, 9)).length, 0, "version 9 accepted");
  assert.equal(unpack(withByte(5, 1)).length, 0, "codec 1 accepted");
  assert.equal(unpack(withByte(6, 1)).length, 0, "dtype 1 accepted");
});

test("any accepted single-byte mutation is a fixed point", () => {
  for (let pos = 0; pos < FULL.length; pos++) {
    for (const mask of [0x01, 0x80, 0xff]) {
      const b = Uint8Array.from(FULL);
      b[pos] ^= mask;
      const out = unpack(b);
      assert.ok(out instanceof Uint8Array);
      if (out.length) {
        JSON.parse(Buffer.from(out).toString("utf8")); // valid JSON
        assert.ok(bytesEqual(unpack(pack(out)), out), `pos ${pos} mask ${mask} not a fixed point`);
      }
    }
  }
});

// ---- synthesize attacker-chosen framing (bypasses pack) ----
function synth(skeletonObj, numArrays, lengths, payload, { meta = new Uint8Array(0), flags = 0 } = {}) {
  const skel = new TextEncoder().encode(JSON.stringify(skeletonObj)); // minified, no whitespace
  const u32 = (v) => Uint8Array.of(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff);
  const parts = [u32(skel.length), skel, u32(numArrays)];
  for (const ln of lengths) parts.push(u32(ln));
  parts.push(payload);
  const head = [
    Uint8Array.of(0x4e, 0x41, 0x4d, 0x5a, 2, 0, 0, flags),
    Uint8Array.of(meta.length & 0xff, (meta.length >>> 8) & 0xff),
    meta,
  ];
  let n = 0;
  for (const p of [...head, ...parts]) n += p.length;
  const out = new Uint8Array(n);
  let o = 0;
  for (const p of [...head, ...parts]) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}
const f32bytes = (x) => new Uint8Array(Float32Array.of(x).buffer);

test("lying numArrays / lengths are rejected", () => {
  assert.equal(unpack(synth({ weights: 0 }, 0xffffffff, [], new Uint8Array(0))).length, 0);
  assert.equal(unpack(synth({ weights: 0 }, 1, [1000], f32bytes(1.5))).length, 0);
  assert.equal(unpack(synth({ weights: 0 }, 1, [1], Uint8Array.of(0, 0, 0))).length, 0); // not mult of 4
});

test("out-of-range weight index rejected", () => {
  assert.equal(unpack(synth({ weights: 5 }, 0, [], new Uint8Array(0))).length, 0);
});

test("corrupt skeleton JSON rejected", () => {
  const skel = new TextEncoder().encode("{not json");
  const u32 = (v) => Uint8Array.of(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff);
  const body = new Uint8Array(4 + skel.length + 4);
  body.set(u32(skel.length), 0);
  body.set(skel, 4);
  const blob = new Uint8Array(10 + body.length);
  blob.set([0x4e, 0x41, 0x4d, 0x5a, 2, 0, 0, 0, 0, 0]);
  blob.set(body, 10);
  assert.equal(unpack(blob).length, 0);
});

test("NaN/Inf inputs rejected by pack", () => {
  for (const s of [
    '{"architecture":"X","weights":[NaN]}',
    '{"architecture":"X","weights":[Infinity]}',
    '{"architecture":"X","weights":[1e400]}',
    '{"architecture":"X","weights":[' + "9".repeat(400) + "]}",
    '{"architecture":"X","gain":1e400,"weights":[0.5]}',
    "not json",
    "",
  ]) {
    assert.equal(pack(new TextEncoder().encode(s)).length, 0, `should reject: ${s.slice(0, 40)}`);
  }
});

test("non-finite payload rejected on decode", () => {
  for (const bad of [
    Uint8Array.of(0, 0, 0xc0, 0x7f), // NaN
    Uint8Array.of(0, 0, 0x80, 0x7f), // +Inf
    Uint8Array.of(0, 0, 0x80, 0xff), // -Inf
  ]) {
    assert.equal(unpack(synth({ weights: 0 }, 1, [1], bad)).length, 0);
  }
});

test("max_json_bytes cap enforced", () => {
  const big = new TextEncoder().encode(
    JSON.stringify({ architecture: "X", weights: Array.from({ length: 5000 }, (_, i) => i) }),
  );
  const packed = pack(big);
  assert.ok(packed.length);
  assert.equal(unpack(packed, 64).length, 0, "over-cap not refused");
  assert.ok(unpack(packed, DEFAULT_MAX_JSON_BYTES).length, "under cap should pass");
});

test("toggling the shuffle flag stays self-consistent", () => {
  const b = Uint8Array.from(FULL);
  b[7] ^= 0x01; // toggle the shuffle flag bit
  const out = unpack(b);
  assert.ok(out instanceof Uint8Array);
  if (out.length) assert.ok(bytesEqual(unpack(pack(out)), out));
});

// ---- the stated rules (NAMZ-FORMAT.md "Placeholders and depth"), held to the reference's own tests ----
const text = (bytes) => new TextDecoder().decode(bytes);
const nested = (depth, inner = "0") => "[".repeat(depth) + inner + "]".repeat(depth);

// A container written by hand: any skeleton, any lengths, `floats` floats (0.5, 1.5, ...) of unshuffled
// payload — the lengths need not sum to it. Version 1 has no metadata block.
function craft(skeleton, lengths, floats, { version = 2, meta = "" } = {}) {
  const enc = new TextEncoder();
  const u16 = (v) => Uint8Array.of(v & 0xff, v >>> 8);
  const u32 = (v) => Uint8Array.of(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, v >>> 24);
  const sk = enc.encode(skeleton);
  const mb = enc.encode(meta);
  const payload = Float32Array.from({ length: floats }, (_, i) => 0.5 + i);
  const parts = [Uint8Array.of(0x4e, 0x41, 0x4d, 0x5a, version, 0, 0, 0)];
  if (version >= 2) parts.push(u16(mb.length), mb);
  parts.push(u32(sk.length), sk, u32(lengths.length), ...lengths.map(u32), new Uint8Array(payload.buffer));
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  parts.reduce((o, p) => (out.set(p, o), o + p.length), 0);
  return out;
}

test("depth: MAX_DEPTH is the reference's kMaxDepth, 128", () => assert.equal(api.MAX_DEPTH, 128));

test("depth: 128 is read and 129 refused — by pack, unpack and readMeta", () => {
  const nam = (d) => `{"weights":[1.5],"x":${nested(d - 1)}}`;
  const p128 = pack(nam(128));
  assert.equal(text(unpack(p128)), nam(128), "pack takes a .nam 128 deep and it reads back");
  assert.equal(pack(nam(129)).length, 0, "pack refuses a .nam 129 deep");
  const sk = (d, inner) => `{"weights":0,"x":${nested(d - 2, inner)}}`; // object + arrays + innermost
  assert.ok(unpack(craft(sk(128, "[]"), [1], 1)).length, "unpack reads a skeleton 128 deep (an empty array is 1)");
  assert.equal(unpack(craft(sk(129, "[]"), [1], 1)).length, 0, "unpack refuses a skeleton 129 deep");
  const one = '{"weights":0}';
  assert.deepEqual(Object.keys(readMeta(craft(one, [1], 1, { meta: `{"a":${nested(127)}}` }))), ["a"]);
  assert.deepEqual(readMeta(craft(one, [1], 1, { meta: `{"a":${nested(128)}}` })), {}, "a meta block 129 deep is no metadata");
});

test("depth: 100,000 deep is refused, never thrown", () => {
  assert.equal(pack(`{"weights":[1.5],"x":${nested(99999)}}`).length, 0);
  assert.equal(unpack(craft(`{"weights":0,"x":${nested(99999)}}`, [1], 1)).length, 0);
  assert.deepEqual(readMeta(craft('{"weights":0}', [1], 1, { meta: `{"a":${nested(32000)}}` })), {});
});

test('pack refuses an integer "weights" member — at any depth, or stamped as metadata', () => {
  for (const nam of [
    '{"weights":7}',
    '{"architecture":"X","config":{"weights":0},"weights":[0.5]}',
    '{"weights":-0}',
    '{"weights":18446744073709551615}',
    '{"a":[{"b":{"weights":-3}}],"weights":[1.5]}',
  ]) {
    assert.equal(pack(nam).length, 0, nam);
  }
  assert.equal(pack('{"weights":[0.5]}', { metadata: { weights: "7" } }).length, 0, "metadata weights=7");
});

test('pack keeps a "weights" that is no integer literal, and it reads back as the reference prints it', () => {
  for (const [nam, back] of [
    ['{"weights":1.0}', '{"weights":1.0}'],
    ['{"weights":0e0}', '{"weights":0.0}'],
    ['{"weights":18446744073709551616}', '{"weights":1.8446744073709552e+19}'],
    ['{"weights":"7"}', '{"weights":"7"}'],
    ['{"weights":true}', '{"weights":true}'],
    ['{"weights":null}', '{"weights":null}'],
    ['{"weights":{"a":1}}', '{"weights":{"a":1}}'],
    ['{"weights":["a",1]}', '{"weights":["a",1]}'],
  ]) {
    assert.equal(text(unpack(pack(nam))), back, nam);
  }
});

test("formatVersion: 1 and 2 are read, 0 is refused", () => {
  const one = '{"weights":0}';
  assert.equal(text(unpack(craft(one, [1], 1, { version: 1 }))), '{"weights":[0.5]}');
  assert.equal(text(unpack(craft(one, [1], 1, { version: 2 }))), '{"weights":[0.5]}');
  assert.equal(unpack(craft(one, [1], 1, { version: 0 })).length, 0, "version 0 accepted");
});

test("lone surrogates: an escaped one is refused wherever it stands, a pair is text", () => {
  for (const s of [
    '{"s":"\\ud800","weights":[0.5]}',
    '{"s":"\\udc00","weights":[0.5]}',
    '{"s":"a\\ud800b","weights":[0.5]}',
    '{"\\udbff":1,"weights":[0.5]}',
    '{"s":"\\ud800","s":"ok","weights":[0.5]}', // before a later duplicate key drops it
    '{"s":"\\udc00\\ud800","weights":[0.5]}', // the wrong way round
  ]) {
    assert.equal(pack(s).length, 0, `should refuse: ${s}`);
  }
  const pair = pack('{"s":"\\ud83c\\udfb8","weights":[0.5]}');
  assert.equal(text(unpack(pair)), '{"s":"🎸","weights":[0.5]}');
  assert.equal(unpack(craft('{"s":"\\ud800","weights":0}', [1], 1)).length, 0);
  assert.deepEqual(readMeta(craft('{"weights":0}', [1], 1, { meta: '{"n":"\\udc00"}' })), {});
});

test("a value a later duplicate key replaces is still read: past every double, 129 deep", () => {
  for (const s of [
    '{"x":1e400,"x":0,"weights":[0.5]}',
    '{"x":' + "9".repeat(400) + ',"x":0,"weights":[0.5]}',
    '{"x":' + nested(128) + ',"x":0,"weights":[0.5]}',
  ]) {
    assert.equal(pack(s).length, 0, `should refuse: ${s.slice(0, 40)}`);
  }
});

test("readMeta reads what the reference reads", () => {
  // every expected value here is what the reference CLI's `map --json` prints for the same block
  const meta = (block) => readMeta(craft('{"weights":0}', [1], 1, { meta: block }));
  assert.deepEqual(meta('{"a":[1,2.5],"b":{"y":1,"x":1e15},"c":[0.25354045629501343,"é"]}'), {
    a: "[1,2.5]",
    b: '{"x":1e+15,"y":1}',
    c: '[0.25354045629501343,"é"]',
  });
  assert.deepEqual(
    meta('{"i":-9223372036854775808,"u":18446744073709551615,"d":18446744073709551616,"f":0.0078125,"g":1e21}'),
    {
      i: "-9223372036854775808",
      u: "18446744073709551615",
      d: "18446744073709551616.000000",
      f: "0.007812", // "%f": an exact tie goes to the even digit
      g: "1000000000000000000000.000000",
    },
  );
  for (const block of ['{"n":"x","big":' + "9".repeat(400) + "}", '{"n":"x","big":1e400}']) {
    assert.deepEqual(meta(block), {}, block.slice(0, 30));
  }
});

test("pack refuses a weight past the float32 range; one that rounds to FLT_MAX is kept", () => {
  assert.equal(pack('{"weights":[0.5,1e39]}').length, 0);
  assert.equal(pack('{"a":{"weights":[-3.5e38]},"weights":[0.5]}').length, 0);
  assert.ok(pack('{"weights":[3.4028235e38]}').length);
});
