// SPDX-License-Identifier: MIT
// The float16 dtype (`.nam16z`): pack writes it only when asked, each weight the half nearest its float32
// (ties to even, subnormals kept), and unpack widens it back exactly. Checked against the test's own rounding
// by arithmetic (helpers.js nearestHalf), which shares nothing with the port's bit code, on every finite half,
// every tie between two halves and the float32 on either side of it.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { formatG3 } from "../src/cli.js";
import { DTYPE_F16, DTYPE_F32, pack, unpack } from "../src/index.js";
import { bytesEqual, nearestHalf } from "./helpers.js";

const enc = (s) => new TextEncoder().encode(s);
const dec = (b) => new TextDecoder().decode(b);
const f32bits = (x) => new Uint32Array(Float32Array.of(x).buffer)[0];
const fromBits = (b) => new Float32Array(Uint32Array.of(b).buffer)[0];
// A finite half's value, by arithmetic: sign * m * 2^-24 below 2^-14, else sign * (1024 + m) * 2^(e - 25).
function halfValue(h) {
  const s = h & 0x8000 ? -1 : 1, e = (h >>> 10) & 0x1f, m = h & 0x3ff;
  return s * (e === 0 ? m * 2 ** -24 : (1024 + m) * 2 ** (e - 25));
}
const literal = (v) => (Object.is(v, -0) ? "-0.0" : String(v)); // "-0" is the integer 0, as in the reference
const namOf = (values) => enc(`{"architecture":"X","weights":[${values.map(literal).join(",")}]}`);
const weightsOf = (text) => Float32Array.from(JSON.parse(dec(text)).weights);
// The last 2n payload bytes of an unshuffled float16 file, as its n halves.
function halvesOf(blob, n) {
  const p = blob.subarray(blob.length - 2 * n);
  return Array.from({ length: n }, (_, i) => p[2 * i] | (p[2 * i + 1] << 8));
}

test("f16: every finite half packs to itself and widens back exactly", () => {
  const halves = [];
  for (let h = 0; h < 0x10000; h++) if (((h >>> 10) & 0x1f) !== 0x1f) halves.push(h);
  const values = halves.map((h) => (h === 0x8000 ? -0 : halfValue(h)));
  const blob = pack(namOf(values), { dtype: DTYPE_F16, shuffle: false });
  assert.ok(blob.length > 0 && blob[6] === DTYPE_F16);
  assert.deepEqual(halvesOf(blob, halves.length), halves);
  const back = weightsOf(unpack(blob));
  for (let i = 0; i < values.length; i++) assert.equal(f32bits(back[i]), f32bits(values[i]), `half ${halves[i]}`);
});

test("f16: every tie between two halves, and the float32 on either side, rounds as arithmetic says", () => {
  const values = [];
  for (let h = 0; h < 0x7bff; h++) {
    const mid = (halfValue(h) + halfValue(h + 1)) / 2; // 12 significant bits: a float32 exactly
    const b = f32bits(mid);
    for (const v of [mid, fromBits(b - 1), fromBits(b + 1)]) values.push(v, -v);
  }
  values.push(fromBits(f32bits(65520) - 1), -fromBits(f32bits(65520) - 1)); // the last float32 below the overflow
  for (const shuffle of [true, false]) {
    const blob = pack(namOf(values), { dtype: DTYPE_F16, shuffle });
    const back = weightsOf(unpack(blob));
    for (let i = 0; i < values.length; i++) {
      assert.equal(f32bits(back[i]), f32bits(nearestHalf(values[i])), `${values[i]} (shuffle ${shuffle})`);
    }
  }
});

test("f16: pack refuses a weight past half's range, and the whole .nam with it", () => {
  for (const w of ["65520", "-65520", "65520.5", "100000", "3.4028234663852886e+38", "1e39"]) {
    const nam = enc(`{"config":{"weights":[0.5]},"weights":[0.25,${w}]}`);
    assert.equal(pack(nam, { dtype: DTYPE_F16 }).length, 0, w);
  }
  assert.equal(pack(enc('{"weights":[65520]}')).length > 0, true, "float32 takes 65520");
  const top = pack(enc('{"weights":[65519.99609375,-65519]}'), { dtype: DTYPE_F16, shuffle: false });
  assert.deepEqual(halvesOf(top, 2), [0x7bff, 0xfbff]);
});

test("f16: dtype is DTYPE_F32 by default and nothing but 0 or 1 is a dtype", () => {
  const nam = enc('{"architecture":"X","weights":[0.1,0.2]}');
  const plain = pack(nam);
  assert.equal(plain[6], DTYPE_F32);
  assert.ok(bytesEqual(pack(nam, { dtype: DTYPE_F32 }), plain));
  assert.ok(bytesEqual(pack(nam, { dtype: undefined }), plain));
  for (const d of [2, -1, 255, "1", true, 1.5, null]) assert.equal(pack(nam, { dtype: d }).length, 0, String(d));
});

test("f16: the planes are every low byte then every high byte; both layouts read the same", () => {
  const nam = enc('{"weights":[1.0,-2.0,0.5]}'); // 0x3c00 0xc000 0x3800
  const a = pack(nam, { dtype: DTYPE_F16 }), b = pack(nam, { dtype: DTYPE_F16, shuffle: false });
  assert.deepEqual(Array.from(a.subarray(-6)), [0x00, 0x00, 0x00, 0x3c, 0xc0, 0x38]);
  assert.deepEqual(Array.from(b.subarray(-6)), [0x00, 0x3c, 0x00, 0xc0, 0x00, 0x38]);
  assert.equal(a[7], 1);
  assert.equal(b[7], 0);
  assert.equal(dec(unpack(a)), '{"weights":[1.0,-2.0,0.5]}');
  assert.equal(dec(unpack(b)), dec(unpack(a)));
});

test("f16: the skeleton and meta block are the float32 file's; repacking is a fixed point", () => {
  const nam = enc('{"architecture":"WaveNet","config":{"weights":[0.3,0.7]},"weights":[0.1,-0.2,0.333]}');
  const metadata = { tone_type: "clean" };
  const p32 = pack(nam, { metadata }), p16 = pack(nam, { metadata, dtype: DTYPE_F16 });
  const head = p32.length - 5 * 4; // everything before the payload
  assert.ok(bytesEqual(p16.subarray(0, 6), p32.subarray(0, 6)));
  assert.ok(bytesEqual(p16.subarray(7, head), p32.subarray(7, head)));
  assert.equal(p16.length, head + 5 * 2);
  assert.ok(bytesEqual(pack(unpack(p16), { metadata, dtype: DTYPE_F16 }), p16), "pack(unpack(p16)) != p16");
  // a .nam packed as float16 and the .nam its float32 file unpacks to, packed as float16: the same file
  assert.ok(bytesEqual(pack(unpack(p32), { metadata, dtype: DTYPE_F16 }), p16));
});

test("f16: unpack refuses an infinite or NaN half, a payload that is no whole number of halves, dtype 2+", () => {
  const head = (n) => Uint8Array.of(0x4e, 0x41, 0x4d, 0x5a, 2, 0, 1, 0, 0, 0, 13, 0, 0, 0, ...enc('{"weights":0}'), 1, 0, 0, 0, n, 0, 0, 0);
  const file = (halves, extra = []) =>
    Uint8Array.of(...head(halves.length), ...halves.flatMap((h) => [h & 0xff, h >>> 8]), ...extra);
  assert.equal(dec(unpack(file([0x3c00, 0x0001]))), '{"weights":[1.0,5.960464477539063e-08]}');
  for (const bad of [0x7c00, 0xfc00, 0x7c01, 0x7e00, 0xffff]) assert.equal(unpack(file([0x3c00, bad])).length, 0, bad.toString(16));
  assert.equal(unpack(file([0x3c00], [0x00])).length, 0, "odd payload");
  assert.equal(unpack(file([0x3c00], [0x00, 0x3c])).length, 0, "one half too many");
  const ok = file([0x3c00]);
  for (let d = 2; d < 256; d++) {
    const b = Uint8Array.from(ok);
    b[6] = d;
    assert.equal(unpack(b).length, 0, `dtype ${d}`);
  }
});

// -- the CLI: --f16 asks, the name agrees ----------------------------------------------------------------
const BIN = join(dirname(fileURLToPath(import.meta.url)), "..", "bin", "namz.js");
// exit code, stdout and stderr, whatever the exit code: map's lossy note and verify's line are stderr after an exit 0
function run(args) {
  const r = spawnSync(process.execPath, [BIN, ...args], { stdio: ["ignore", "pipe", "pipe"] });
  return { status: r.status, out: r.stdout.toString(), err: r.stderr.toString() };
}

test("f16 cli: --f16 writes a .nam16z, and either name mismatch is refused before anything is written", () => {
  const d = mkdtempSync(join(tmpdir(), "namzjs16-"));
  const nam = join(d, "in.nam");
  writeFileSync(nam, '{"architecture":"X","weights":[0.1,0.2]}');
  assert.equal(run(["encode", nam, join(d, "a.nam16z"), "--f16"]).status, 0);
  assert.equal(readFileSync(join(d, "a.nam16z"))[6], DTYPE_F16);
  assert.equal(run(["encode", nam, join(d, "b.NAM16Z"), "--f16", "--no-shuffle"]).status, 0);
  const r1 = run(["encode", nam, join(d, "c.namz"), "--f16"]);
  assert.equal(r1.status, 2);
  assert.match(r1.err, /name the output \.nam16z/);
  const r2 = run(["encode", nam, join(d, "e.nam16z")]);
  assert.equal(r2.status, 2);
  assert.match(r2.err, /pass --f16/);
  assert.ok(!existsSync(join(d, "c.namz")) && !existsSync(join(d, "e.nam16z")));
  writeFileSync(join(d, "big.nam"), '{"weights":[65520]}');
  const r3 = run(["encode", join(d, "big.nam"), join(d, "big.nam16z"), "--f16"]);
  assert.equal(r3.status, 1);
  assert.match(r3.err, /65520/);
  const m = run(["map", join(d, "a.nam16z")]);
  assert.equal(m.status, 0);
  assert.ok(m.err.endsWith("(float16 weights — lossy)\n"), m.err);
  // the reference prints, for these two weights: "in.nam: OK  raw=40 namz=62 nam16z=58 (145.0%)  idempotent=yes  max|w16-w32|=4.88e-05"
  const v16 = run(["verify", nam, "--f16"]);
  assert.equal(v16.status, 0);
  assert.equal(v16.err, `${nam}: OK  raw=40 namz=62 nam16z=58 (145.0%)  idempotent=yes  max|w16-w32|=4.88e-05\n`);
  const v = run(["verify", nam, "--bogus"]);
  assert.equal(v.status, 2);
  assert.match(v.err, /unknown option: --bogus/);
});

test("f16 cli: max|w16-w32| is printed as C's %.3g", () => {
  // printf("%.3g") of each, by the C library: ties go to even on the exact value
  const cases = [[0, "0"], [0.03125, "0.0312"], [0.0009765625, "0.000977"], [1.52587890625e-5, "1.53e-05"],
    [0.5, "0.5"], [16, "16"], [1000, "1e+03"], [999.5, "1e+03"], [0.0001, "0.0001"], [0.00009995, "0.0001"],
    [123456, "1.23e+05"], [1.0625, "1.06"], [1.0675, "1.07"], [2.98023223876953125e-8, "2.98e-08"], [1e-300, "1e-300"]];
  for (const [x, want] of cases) assert.equal(formatG3(x), want, String(x));
});
