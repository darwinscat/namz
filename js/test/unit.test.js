// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { isNamz, pack, readMeta, unpack } from "../src/index.js";

const BIN = join(dirname(fileURLToPath(import.meta.url)), "..", "bin", "namz.js");
const enc = (s) => new TextEncoder().encode(s);
const dec = (b) => Buffer.from(b).toString("utf8");

test("isNamz", () => {
  assert.ok(isNamz(enc("NAMZ....")));
  assert.ok(!isNamz(enc("XAMZ")));
  assert.ok(!isNamz(enc("NAM")));
  assert.ok(!isNamz(new Uint8Array(0)));
});

test("pack accepts string and bytes identically", () => {
  const a = pack('{"architecture":"X","weights":[0.5]}');
  const b = pack(enc('{"architecture":"X","weights":[0.5]}'));
  assert.ok(a.length && a.length === b.length && a.every((x, i) => x === b[i]));
});

test("roundtrip minifies and sorts keys", () => {
  const out = unpack(pack(enc('{"b":2,"architecture":"X","weights":[1.0,2.0],"a":1}')));
  assert.equal(dec(out), '{"a":1,"architecture":"X","b":2,"weights":[1.0,2.0]}');
});

test("integer-valued float non-weight keeps its decimal (nlohmann parity)", () => {
  assert.equal(dec(unpack(pack(enc('{"loudness":-18.0,"weights":[1.0]}')))), '{"loudness":-18.0,"weights":[1.0]}');
});

test("empty weights array is preserved", () => {
  assert.equal(dec(unpack(pack(enc('{"architecture":"X","weights":[]}')))), '{"architecture":"X","weights":[]}');
});

test("oversized ints coerced to double like the reference", () => {
  assert.ok(dec(unpack(pack(enc('{"x":18446744073709551616,"weights":[1.0]}')))).includes('"x":1.8446744073709552e+19'));
  const out = dec(unpack(pack(enc('{"a":18446744073709551615,"b":-9223372036854775808,"weights":[1.0]}'))));
  assert.ok(out.includes('"a":18446744073709551615'));
  assert.ok(out.includes('"b":-9223372036854775808'));
});

test("read_meta typed fields", () => {
  const blob = pack(enc('{"architecture":"WaveNet","weights":[0.5]}'), {
    metadata: { tone_type: "hi-gain", boost: "true", count: "42", device: "tube:1" },
  });
  assert.deepEqual(readMeta(blob), { tone_type: "hi-gain", boost: "true", count: "42", device: "tube:1" });
  const obj = JSON.parse(dec(unpack(blob)));
  assert.equal(obj.metadata.boost, true);
  assert.equal(obj.metadata.count, 42);
});

test("read_meta empty for non-namz and v1", () => {
  assert.deepEqual(readMeta(enc("not a namz")), {});
  assert.deepEqual(readMeta(Uint8Array.of(0x4e, 0x41, 0x4d, 0x5a, 1, 0, 0, 0, 0x2e, 0x2e)), {});
  assert.deepEqual(readMeta(pack(enc('{"architecture":"X","weights":[1.0]}'))), {});
});

test("pack without metadata has no header block", () => {
  const blob = pack(enc('{"architecture":"X","weights":[1.0]}'));
  assert.equal(blob[8], 0);
  assert.equal(blob[9], 0);
});

test("U+2028/U+2029 stay raw in strings (nlohmann parity, not escaped)", () => {
  const s2028 = String.fromCharCode(0x2028), s2029 = String.fromCharCode(0x2029);
  const out = dec(unpack(pack(enc(`{"s":"a${s2028}b${s2029}c","weights":[1.0]}`))));
  assert.ok(out.includes(`a${s2028}b${s2029}c`), "separators must round-trip raw");
  assert.ok(!out.includes("\\u2028") && !out.includes("\\u2029"), "must not escape separators");
});

test("leading-zero metadata typed as int like std::stoll", () => {
  const blob = pack(enc('{"architecture":"X","weights":[1.0]}'), {
    metadata: { count: "000000000000000000001", zeros: "0000" },
  });
  assert.equal(readMeta(blob).count, "1");
  assert.equal(readMeta(blob).zeros, "0");
  assert.ok(dec(unpack(blob)).includes('"count":1'), "integer in skeleton, not a string");
});

test("public API never throws on non-bytes input", () => {
  for (const bad of [null, undefined, 42, {}, [1, 2, 3], true]) {
    assert.equal(isNamz(bad), false);
    assert.equal(unpack(bad).length, 0);
    assert.equal(pack(bad).length, 0);
    assert.deepEqual(readMeta(bad), {});
  }
});

// ---- CLI (real bin) ----
function tmp() {
  return mkdtempSync(join(tmpdir(), "namzjs-"));
}
function cli(args, opts = {}) {
  return execFileSync(process.execPath, [BIN, ...args], { encoding: "utf8", ...opts });
}

test("cli encode/decode roundtrip", () => {
  const d = tmp();
  const nam = join(d, "in.nam"), out = join(d, "out.namz"), back = join(d, "back.nam");
  writeFileSync(nam, '{"architecture":"WaveNet","weights":[0.5,-0.25,0.125]}');
  cli(["encode", nam, out], { stdio: ["ignore", "ignore", "ignore"] });
  assert.ok(isNamz(new Uint8Array(readFileSync(out))));
  cli(["decode", out, back], { stdio: ["ignore", "ignore", "ignore"] });
  assert.equal(readFileSync(back, "utf8"), '{"architecture":"WaveNet","weights":[0.5,-0.25,0.125]}');
});

test("cli --no-shuffle differs in flag but decodes the same", () => {
  const d = tmp();
  const nam = join(d, "in.nam"), a = join(d, "a.namz"), b = join(d, "b.namz");
  writeFileSync(nam, '{"architecture":"X","weights":[0.5,-0.5,0.25]}');
  cli(["encode", nam, a], { stdio: ["ignore", "ignore", "ignore"] });
  cli(["encode", nam, b, "--no-shuffle"], { stdio: ["ignore", "ignore", "ignore"] });
  const A = new Uint8Array(readFileSync(a)), B = new Uint8Array(readFileSync(b));
  assert.equal(A[7], 1);
  assert.equal(B[7], 0);
  assert.equal(dec(unpack(A)), dec(unpack(B)));
});

test("cli map --json", () => {
  const d = tmp();
  const nam = join(d, "in.nam"), out = join(d, "out.namz");
  writeFileSync(nam, '{"architecture":"X","weights":[1.0]}');
  cli(["encode", nam, out, "--set", "tone_type=clean", "--set", "boost=true"], {
    stdio: ["ignore", "ignore", "ignore"],
  });
  const stdout = cli(["map", out, "--json"], { stdio: ["ignore", "pipe", "ignore"] });
  assert.deepEqual(JSON.parse(stdout.trim()), { boost: "true", tone_type: "clean" });
});

test("cli verify OK; bad --set and bogus command exit 2", () => {
  const d = tmp();
  const nam = join(d, "in.nam");
  writeFileSync(nam, '{"architecture":"X","weights":[0.1,0.2,0.3]}');
  cli(["verify", nam], { stdio: ["ignore", "ignore", "ignore"] }); // exits 0, no throw
  const fails = (args) => {
    try {
      cli(args, { stdio: ["ignore", "ignore", "ignore"] });
      return 0;
    } catch (e) {
      return e.status;
    }
  };
  assert.equal(fails(["encode", nam, join(d, "o.namz"), "--set", "noequals"]), 2);
  assert.equal(fails([]), 2);
  assert.equal(fails(["bogus"]), 2);
});
