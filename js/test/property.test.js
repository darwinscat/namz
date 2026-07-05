// SPDX-License-Identifier: MIT
// Local property fuzz: random finite float32 → round-trip → bit-exact + idempotent (both shuffle modes).
import assert from "node:assert/strict";
import test from "node:test";
import { isNamz, pack, unpack } from "../src/index.js";
import { bytesEqual } from "./helpers.js";

// A tiny deterministic PRNG so runs are reproducible.
function mulberry32(seed) {
  return function () {
    seed |= 0;
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function randomFiniteF32(rng, n) {
  const out = new Float32Array(n);
  const u = new Uint32Array(1);
  const f = new Float32Array(u.buffer);
  let i = 0;
  while (i < n) {
    u[0] = (rng() * 4294967296) >>> 0;
    if (Number.isFinite(f[0])) out[i++] = f[0];
  }
  return out;
}

function namWith(a0, a1) {
  const obj = {
    architecture: "WaveNet",
    sample_rate: 48000,
    config: { submodels: [{ max_value: 0.5, model: { architecture: "WaveNet", weights: [...a0] } }] },
    weights: [...a1],
  };
  return new TextEncoder().encode(JSON.stringify(obj));
}

for (const shuffle of [true, false]) {
  for (let seed = 0; seed < 12; seed++) {
    test(`roundtrip bit-exact seed=${seed} shuffle=${shuffle}`, () => {
      const rng = mulberry32(seed * 7 + (shuffle ? 1 : 0));
      const a0 = randomFiniteF32(rng, Math.floor(rng() * 300));
      const a1 = randomFiniteF32(rng, Math.floor(rng() * 300));
      const src = namWith(a0, a1);

      const packed = pack(src, { shuffle });
      assert.ok(packed.length && isNamz(packed));
      const back = unpack(packed);
      assert.ok(back.length);

      const obj = JSON.parse(Buffer.from(back).toString("utf8"));
      const got0 = Float32Array.from(obj.config.submodels[0].model.weights);
      const got1 = Float32Array.from(obj.weights);
      assert.ok(bytesEqual(new Uint8Array(got0.buffer), new Uint8Array(a0.buffer)), "inner weights");
      assert.ok(bytesEqual(new Uint8Array(got1.buffer), new Uint8Array(a1.buffer)), "outer weights");

      // idempotence
      assert.ok(bytesEqual(pack(back, { shuffle }), packed), "not idempotent");
    });
  }
}

test("special float32 values survive bit-exact", () => {
  const specials = Float32Array.from([
    0, -0, 1.401298464324817e-45, 1.1754943508222875e-38, 3.4028234663852886e38,
    -3.4028234663852886e38, 1 / 3,
  ]);
  // Build the input by hand: JSON.stringify(-0) drops the sign ("0"), which would corrupt the input
  // before it ever reaches the codec. The codec itself preserves -0.0 (see the edge-floats vector).
  const weightStrings = [...specials].map((v) => (Object.is(v, -0) ? "-0.0" : String(v)));
  const src = new TextEncoder().encode(`{"architecture":"X","weights":[${weightStrings.join(",")}]}`);
  for (const shuffle of [true, false]) {
    const back = unpack(pack(src, { shuffle }));
    const got = Float32Array.from(JSON.parse(Buffer.from(back).toString("utf8")).weights);
    assert.ok(bytesEqual(new Uint8Array(got.buffer), new Uint8Array(specials.buffer)));
  }
});
