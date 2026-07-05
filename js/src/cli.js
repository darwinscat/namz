// SPDX-License-Identifier: MIT
// namz — command-line packer/unpacker, mirroring the C++ reference CLI (cli/namz.cpp).
import { readFileSync, writeFileSync } from "node:fs";
import { DEFAULT_MAX_JSON_BYTES, isNamz, pack, readMeta, unpack } from "./index.js";

const USAGE = `namz — lossless .nam <-> .namz codec

Usage:
  namz encode <in.nam> <out.namz> [--no-shuffle] [--set key=value ...]
  namz decode <in.namz> <out.nam>
  namz map    <in.namz> [--json]        print the metadata header (no weight decode)
  namz verify <in.nam>                  pack->unpack round-trip check + ratio
`;

function usage() {
  process.stderr.write(USAGE);
  return 2;
}

function read(path) {
  try {
    return readFileSync(path);
  } catch {
    return null;
  }
}
function write(path, data) {
  try {
    writeFileSync(path, data);
    return true;
  } catch {
    return false;
  }
}
function pct(numer, denom) {
  return denom === 0 ? 0 : (100 * numer) / denom;
}

function doEncode(argv) {
  if (argv.length < 4) return usage();
  const inPath = argv[2];
  const outPath = argv[3];
  const options = { shuffle: true, metadata: {} };
  for (let i = 4; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--no-shuffle") {
      options.shuffle = false;
    } else if (a === "--set" && i + 1 < argv.length) {
      const kv = argv[++i];
      const eq = kv.indexOf("=");
      if (eq < 0) {
        process.stderr.write(`bad --set (need key=value): ${kv}\n`);
        return 2;
      }
      options.metadata[kv.slice(0, eq)] = kv.slice(eq + 1);
    } else {
      process.stderr.write(`unknown option: ${a}\n`);
      return usage();
    }
  }
  const src = read(inPath);
  if (src === null) {
    process.stderr.write(`cannot read ${inPath}\n`);
    return 1;
  }
  const packed = pack(src, options);
  if (packed.length === 0) {
    process.stderr.write(`encode failed (not valid NAM JSON?): ${inPath}\n`);
    return 1;
  }
  if (!write(outPath, packed)) {
    process.stderr.write(`cannot write ${outPath}\n`);
    return 1;
  }
  process.stderr.write(
    `${inPath} -> ${outPath}  (${src.length} -> ${packed.length} bytes, ${pct(packed.length, src.length).toFixed(1)}%)\n`,
  );
  return 0;
}

function doDecode(argv) {
  if (argv.length < 4) return usage();
  const inPath = argv[2];
  const outPath = argv[3];
  const src = read(inPath);
  if (src === null) {
    process.stderr.write(`cannot read ${inPath}\n`);
    return 1;
  }
  const nam = unpack(src, DEFAULT_MAX_JSON_BYTES);
  if (nam.length === 0) {
    process.stderr.write(`decode failed (not a .namz / corrupt / over cap): ${inPath}\n`);
    return 1;
  }
  if (!write(outPath, nam)) {
    process.stderr.write(`cannot write ${outPath}\n`);
    return 1;
  }
  process.stderr.write(`${inPath} -> ${outPath}  (${src.length} -> ${nam.length} bytes)\n`);
  return 0;
}

function doMap(argv) {
  if (argv.length < 3) return usage();
  const inPath = argv[2];
  const asJson = argv.length >= 4 && argv[3] === "--json";
  const src = read(inPath);
  if (src === null) {
    process.stderr.write(`cannot read ${inPath}\n`);
    return 1;
  }
  if (!isNamz(src)) {
    process.stderr.write(`not a .namz: ${inPath}\n`);
    return 1;
  }
  const m = readMeta(src);
  const keys = Object.keys(m).sort();
  if (asJson) {
    const esc = (s) => s.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
    process.stdout.write(`{${keys.map((k) => `"${esc(k)}":"${esc(m[k])}"`).join(",")}}\n`);
  } else {
    for (const k of keys) process.stdout.write(`  ${k} = ${m[k]}\n`);
    if (keys.length === 0)
      process.stderr.write("(no metadata header — v1 .namz or packed without --set)\n");
  }
  return 0;
}

function doVerify(argv) {
  if (argv.length < 3) return usage();
  const inPath = argv[2];
  const src = read(inPath);
  if (src === null) {
    process.stderr.write(`cannot read ${inPath}\n`);
    return 1;
  }
  const packed = pack(src);
  if (packed.length === 0) {
    process.stderr.write(`FAIL pack: ${inPath}\n`);
    return 1;
  }
  const back = unpack(packed, DEFAULT_MAX_JSON_BYTES);
  if (back.length === 0) {
    process.stderr.write(`FAIL unpack: ${inPath}\n`);
    return 1;
  }
  const packed2 = pack(back);
  const idempotent = packed.length === packed2.length && packed.every((b, i) => b === packed2[i]);
  process.stderr.write(
    `${inPath}: ${idempotent ? "OK" : "MISMATCH"}  raw=${src.length} namz=${packed.length} (${pct(packed.length, src.length).toFixed(1)}%)  idempotent=${idempotent ? "yes" : "no"}\n`,
  );
  return idempotent ? 0 : 1;
}

export function main(argv) {
  if (argv.length < 2) return usage();
  const cmd = argv[1];
  if (cmd === "encode") return doEncode(argv);
  if (cmd === "decode") return doDecode(argv);
  if (cmd === "map") return doMap(argv);
  if (cmd === "verify") return doVerify(argv);
  return usage();
}
