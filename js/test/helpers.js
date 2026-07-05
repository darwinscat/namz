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
