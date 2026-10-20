// SPDX-License-Identifier: MIT
import { existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));

/** Walk up until we find conformance/manifest.json (monorepo: ../conformance). */
export function conformanceDir() {
  let d = HERE;
  for (let i = 0; i < 6; i++) {
    if (existsSync(join(d, "conformance", "manifest.json"))) return join(d, "conformance");
    d = dirname(d);
  }
  return null;
}

export function readManifest() {
  const dir = conformanceDir();
  return dir ? JSON.parse(readFileSync(join(dir, "manifest.json"), "utf8")) : null;
}

export function readVector(rel) {
  return readFileSync(join(conformanceDir(), rel));
}

export function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

// The half nearest a float32 `f`, ties to even, as a number — by arithmetic on doubles, sharing nothing with
// the port's bit code: a half's step at |f| in [2^(e-1), 2^e) is 2^(e-11), never finer than 2^-24, and
// scaling by a power of two is exact.
export function nearestHalf(f) {
  if (f === 0) return f;
  let e = Math.floor(Math.log2(Math.abs(f))) + 1;
  if (2 ** (e - 1) > Math.abs(f)) e--; // log2 may land one off near a power of two
  else if (2 ** e <= Math.abs(f)) e++;
  const q = Math.max(e - 11, -24);
  const x = f / 2 ** q;
  const fl = Math.floor(x);
  const d = x - fl;
  const r = d > 0.5 || (d === 0.5 && fl % 2 !== 0) ? fl + 1 : fl;
  return r === 0 ? (f < 0 ? -0 : 0) : r * 2 ** q;
}
