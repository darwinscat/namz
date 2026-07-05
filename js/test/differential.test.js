// SPDX-License-Identifier: MIT
// Optional differential test: pack() must be byte-identical to the C++ reference CLI.
// Skipped unless a built reference CLI is found ($NAMZ_CLI or <repo>/build/namz). The committed
// conformance vectors already cover CI; this is a live cross-check for anyone with the C++ built.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { pack } from "../src/index.js";
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
  // leading-zero metadata must be typed by value, not string length
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
