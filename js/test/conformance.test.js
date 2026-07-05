// SPDX-License-Identifier: MIT
// The cross-language TCK: encode byte-match, decode bit-exact float32, reject invalid, idempotence.
import assert from "node:assert/strict";
import test from "node:test";
import { isNamz, pack, unpack } from "../src/index.js";
import { bytesEqual, conformanceDir, readManifest, readVector } from "./helpers.js";

const manifest = readManifest();

test("conformance vectors present", () => {
  assert.ok(conformanceDir(), "conformance/ vectors must be findable");
  assert.equal(manifest.format, "namz");
  assert.ok(manifest.valid.length && manifest.invalid.length);
});

// Collect numeric "weights" arrays in sorted-DFS order as Float32Array (independent reference).
function gatherWeights(node, out) {
  if (node && typeof node === "object" && !Array.isArray(node)) {
    for (const key of Object.keys(node).sort()) {
      const val = node[key];
      const numeric =
        Array.isArray(val) && val.every((x) => typeof x === "number" && Number.isFinite(x));
      if (key === "weights" && numeric) out.push(Float32Array.from(val));
      else gatherWeights(val, out);
    }
  } else if (Array.isArray(node)) {
    for (const item of node) gatherWeights(item, out);
  }
}

for (const c of manifest?.valid ?? []) {
  test(`valid: ${c.name}`, () => {
    const src = readVector(c.input);
    const expected = readVector(c.output);
    const options = { shuffle: c.shuffle !== false, metadata: c.set || {} };

    // 1. encode byte-match
    const got = pack(src, options);
    assert.ok(bytesEqual(got, expected), `encode byte mismatch (${got.length} vs ${expected.length})`);

    // 2. decode bit-exact float32 vs the input weights
    const decoded = unpack(expected);
    assert.ok(decoded.length > 0, "decode returned empty");
    const want = [];
    const gotW = [];
    gatherWeights(JSON.parse(Buffer.from(src).toString("utf8")), want);
    gatherWeights(JSON.parse(Buffer.from(decoded).toString("utf8")), gotW);
    assert.equal(want.length, gotW.length, "weight-array count differs");
    for (let i = 0; i < want.length; i++) {
      assert.ok(
        bytesEqual(new Uint8Array(want[i].buffer), new Uint8Array(Float32Array.from(gotW[i]).buffer)),
        "weights not bit-exact",
      );
    }

    // 3. decode → encode reproduces the golden (idempotence)
    assert.ok(bytesEqual(pack(decoded, options), expected), "not idempotent");
  });
}

for (const c of manifest?.invalid ?? []) {
  test(`invalid rejected: ${c.name}`, () => {
    const blob = readVector(c.file);
    assert.equal(unpack(blob).length, 0, `should be rejected: ${c.why}`);
  });
}
