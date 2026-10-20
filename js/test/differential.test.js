// SPDX-License-Identifier: MIT
// Optional differential test: pack(), unpack() and readMeta() must be byte-identical to the C++ reference CLI.
// Skipped unless a built reference CLI is found ($NAMZ_CLI or <repo>/build/namz). The committed
// conformance vectors already cover CI; this is a live cross-check for anyone with the C++ built.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { pack, readMeta, unpack } from "../src/index.js";
import { bytesEqual } from "./helpers.js";

function findCli() {
  if (process.env.NAMZ_CLI && existsSync(process.env.NAMZ_CLI)) return process.env.NAMZ_CLI;
  let d = dirname(fileURLToPath(import.meta.url));
  for (let i = 0; i < 5; i++) {
    const c = join(d, "build", "namz");
    if (existsSync(c)) return c;
    d = dirname(d);
  }
  return null;
}
const CLI = findCli();
const TMP = CLI ? mkdtempSync(join(tmpdir(), "namzjsdiff-")) : null;
const enc = (s) => new TextEncoder().encode(s);

function refEncode(namBytes, shuffle, sets) {
  const inp = join(TMP, "in.nam"), out = join(TMP, "out.namz");
  writeFileSync(inp, namBytes);
  if (existsSync(out)) rmSync(out);
  const args = ["encode", inp, out];
  if (!shuffle) args.push("--no-shuffle");
  for (const [k, v] of Object.entries(sets || {})) args.push("--set", `${k}=${v}`);
  try {
    execFileSync(CLI, args, { stdio: ["ignore", "ignore", "ignore"] });
  } catch {
    return null;
  }
  return existsSync(out) ? new Uint8Array(readFileSync(out)) : null;
}

function check(namBytes, shuffle = true, sets = null) {
  const ref = refEncode(namBytes, shuffle, sets);
  const mine = pack(namBytes, { shuffle, metadata: sets || {} });
  assert.equal(ref === null, mine.length === 0, `accept/reject differs: ${Buffer.from(namBytes).toString().slice(0, 60)}`);
  if (ref) assert.ok(bytesEqual(ref, mine), `byte mismatch: ${Buffer.from(namBytes).toString().slice(0, 80)}`);
}

const TARGETED = [
  '{"sample_rate":48000.0,"weights":[1.0,2.0]}',
  '{"loudness":-18.0,"x":0.1,"y":0.3333333333333333,"weights":[0.5]}',
  '{"x":18446744073709551616,"weights":[1.0]}', // exact 2^64 -> double, scientific
  '{"a":9223372036854775807,"b":18446744073709551615,"weights":[1.0]}',
  '{"ключ":"значение","emoji":"🎸","weights":[1.0]}',
  '{"s":"a\\tb\\nc\\u0001\\"\\\\","weights":[1.0]}',
  '{"weights":[0,1,-1,16777216,-16777216]}',
  "[1,2,3]",
  '{"config":{"z":1,"a":2,"m":[{"weights":[1.0]},{"weights":[]}]},"weights":[9.0]}',
  // digits nlohmann's Grisu2 picks its own way, and its switch to exponent form past 15 integer digits
  '{"a":-0.06020416246376235,"b":-198.70986938476562,"c":1e15,"d":2e15,"e":1e-5,"weights":[0.5]}',
  '{"a":123456789012345678901234567890,"b":1.2345678901234567e300,"c":-4.9406564584124654e-324,"weights":[0.5]}',
  // refused before a duplicate key drops the value: a number past every double, a lone surrogate, depth
  '{"x":1e400,"x":0,"weights":[0.5]}',
  '{"x":' + "9".repeat(400) + ',"x":0,"weights":[0.5]}',
  '{"s":"\\ud800","s":"ok","weights":[0.5]}',
  '{"x":' + "[".repeat(128) + "0" + "]".repeat(128) + ',"x":0,"weights":[0.5]}',
  // a weight past the float32 range
  '{"weights":[0.5,1e39]}',
];

test("differential: targeted cases byte-identical", { skip: CLI ? false : "reference CLI not built" }, () => {
  for (const j of TARGETED) {
    check(enc(j), true);
    check(enc(j), false);
  }
  check(enc('{"architecture":"X","weights":[0.5]}'), true, {
    tone_type: "hi-gain", boost: "true", n: "42", device: "tube:1,pnp:1",
  });
  check(enc("[1,2,3]"), true, { foo: "bar" }); // both reject
  // U+2028 / U+2029 must stay raw (nlohmann does not escape them)
  const seps = String.fromCharCode(0x2028) + "y" + String.fromCharCode(0x2029);
  check(enc(`{"s":"a${seps}b","weights":[1.0]}`), true);
  // leading-zero metadata stays a string on both sides — the zeros are part of an identifier
  check(enc('{"architecture":"X","weights":[0.5]}'), true, { count: "000000000000000000001", z: "0000" });
});

test("differential: random fuzz byte-identical", { skip: CLI ? false : "reference CLI not built" }, () => {
  let seed = 0xc0ffee;
  const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff), seed / 0x7fffffff);
  const ri = (a, b) => a + Math.floor(rand() * (b - a + 1));
  const rf = () => {
    const k = rand();
    if (k < 0.4) return ri(-1000000, 1000000);
    if (k < 0.7) return Math.round((rand() * 2000 - 1000) * 1e6) / 1e6;
    return [0.0, -0.0, 0.5, 1 / 3, -18.0, 48000.0][ri(0, 5)];
  };
  const robj = (depth = 0) => {
    const o = {};
    for (let i = ri(0, 4); i > 0; i--) {
      const key = ["architecture", "sample_rate", "gain", "cfg", "zzz", "aaa"][ri(0, 5)];
      const r = rand();
      if (r < 0.35) o[key] = rand() < 0.6 ? ri(-100000, 100000) : rf();
      else if (r < 0.55) o[key] = ["WaveNet", "LSTM", "X"][ri(0, 2)];
      else if (r < 0.65) o[key] = [true, false, null][ri(0, 2)];
      else if (r < 0.8 && depth < 3) o[key] = robj(depth + 1);
      else o[key] = null;
    }
    if (rand() < 0.7) {
      o.weights = [];
      for (let i = ri(0, 40); i > 0; i--) o.weights.push(rand() < 0.5 ? rf() : ri(-100, 100));
    }
    return o;
  };
  for (let it = 0; it < 400; it++) check(enc(JSON.stringify(robj())), rand() < 0.5);
});

function refRun(args, blob) {
  const inp = join(TMP, "in.namz"), out = join(TMP, "out.nam");
  writeFileSync(inp, blob);
  if (existsSync(out)) rmSync(out);
  try {
    return execFileSync(CLI, [args[0], inp, ...args.slice(1).map((a) => (a === "OUT" ? out : a))], {
      stdio: ["ignore", "pipe", "ignore"],
    });
  } catch {
    return null;
  }
}

function container(skeleton, floats, meta = new Uint8Array(0)) {
  const u32 = (v) => Uint8Array.of(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, v >>> 24);
  const sk = enc(skeleton);
  const parts = [Uint8Array.of(0x4e, 0x41, 0x4d, 0x5a, 2, 0, 0, 0, meta.length & 0xff, meta.length >>> 8), meta];
  parts.push(u32(sk.length), sk, u32(1), u32(floats.length), new Uint8Array(floats.buffer));
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  parts.reduce((o, p) => (out.set(p, o), o + p.length), 0);
  return out;
}

test("differential: unpack of 100,000 random float32 values is byte-identical", { skip: CLI ? false : "reference CLI not built" }, () => {
  // the reference prints each with Grisu2's digits, which are not always the shortest: about one in four
  // hundred differs from what toExponential() gives
  let seed = 0x5eed;
  const next = () => ((seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0), seed);
  const bits = new Uint32Array(100000);
  for (let i = 0; i < bits.length; i++) {
    let b;
    do b = next(); while ((b & 0x7f800000) === 0x7f800000); // no NaN or infinity
    bits[i] = b;
  }
  const blob = container('{"weights":0}', new Float32Array(bits.buffer));
  assert.ok(refRun(["decode", "OUT"], blob) !== null);
  const ref = new Uint8Array(readFileSync(join(TMP, "out.nam")));
  assert.ok(bytesEqual(unpack(blob), ref), "unpack bytes differ from the reference");
});

test("differential: readMeta matches the reference", { skip: CLI ? false : "reference CLI not built" }, () => {
  for (const block of [
    '{"a":[1,2.5],"b":{"y":1,"x":1e15},"c":[0.25354045629501343,"é"],"d":null}',
    '{"i":-9223372036854775808,"u":18446744073709551615,"d":18446744073709551616,"f":0.0078125,"g":-2.5e-7,"h":1e21}',
    '{"n":"x","big":' + "9".repeat(400) + "}",
    '{"n":"x","big":1e400}',
    '{"n":"\\ud800"}',
    '{"a":' + "[".repeat(128) + "0" + "]".repeat(128) + ',"a":0,"name":"n"}',
  ]) {
    const blob = container('{"weights":0}', Float32Array.of(0.5), enc(block));
    const ref = JSON.parse(refRun(["map", "--json"], blob).toString());
    assert.deepEqual({ ...readMeta(blob) }, ref, block.slice(0, 50));
  }
});
