// SPDX-License-Identifier: MIT
// namz — command-line packer/unpacker, mirroring the C++ reference CLI (cli/namz.cpp).
import { readFileSync, writeFileSync } from "node:fs";
import { DEFAULT_MAX_JSON_BYTES, DTYPE_F16, isNamz, pack, readMeta, unpack } from "./index.js";
import { NamNum, parse } from "./json.js";

const USAGE = `namz — lossless .nam <-> .namz codec

Usage:
  namz encode <in.nam> <out.namz> [--no-shuffle] [--set key=value ...]
  namz encode <in.nam> <out.nam16z> --f16 [--no-shuffle] [--set key=value ...]
                                        float16 weights: half the size, LOSSY
  namz decode <in.namz|in.nam16z> <out.nam>
  namz map    <in.namz|in.nam16z> [--json]   print the metadata header (no weight decode)
  namz verify <in.nam> [--f16]          pack->unpack round-trip check + ratio
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

// True if `path` ends in ".nam16z", ASCII case ignored — the name a float16 file goes by.
function namedF16(path) {
  return path.length >= 7 && path.slice(-7).replace(/[A-Z]/g, (c) => c.toLowerCase()) === ".nam16z";
}

// C's "%.3g" of a finite non-negative double: 3 significant digits rounded from the exact value, ties to
// even; fixed while the exponent is within [-4, 3), else d.dde±XX; trailing zeros dropped.
function formatG3(x) {
  if (x === 0) return "0";
  const dv = new DataView(new ArrayBuffer(8));
  dv.setFloat64(0, x);
  const hi = dv.getUint32(0);
  const be = (hi >>> 20) & 0x7ff;
  let m = (BigInt(hi & 0xfffff) << 32n) | BigInt(dv.getUint32(4));
  if (be !== 0) m |= 1n << 52n;
  const e2 = (be === 0 ? 1 : be) - 1075; // x = m * 2^e2 exactly
  // the exact decimal digits of x: x = digits * 10^e10
  const digits = e2 >= 0 ? (m << BigInt(e2)).toString() : (m * 5n ** BigInt(-e2)).toString();
  const e10 = e2 >= 0 ? 0 : e2;
  let exp = digits.length - 1 + e10; // decimal exponent of the first digit
  let head = BigInt(digits.slice(0, 3).padEnd(3, "0"));
  const tail = digits.slice(3);
  if (tail.length && (tail[0] > "5" || (tail[0] === "5" && (/[1-9]/.test(tail.slice(1)) || head % 2n === 1n)))) {
    head += 1n;
    if (head === 1000n) {
      head = 100n;
      exp += 1;
    }
  }
  const sig = head.toString(); // 3 digits
  const strip = (s) => (s.includes(".") ? s.replace(/0+$/, "").replace(/\.$/, "") : s);
  if (exp < -4 || exp >= 3) {
    const e = Math.abs(exp);
    return `${strip(`${sig[0]}.${sig.slice(1)}`)}e${exp < 0 ? "-" : "+"}${e < 10 ? "0" : ""}${e}`;
  }
  if (exp >= 0) return strip(`${sig.slice(0, exp + 1)}.${sig.slice(exp + 1)}`);
  return strip(`0.${"0".repeat(-exp - 1)}${sig}`);
}

// Every numeric "weights" array of a parsed `.nam`, as numbers, in pack's order (sorted keys, depth first).
function weightsOf(node, out) {
  if (Array.isArray(node)) {
    for (const item of node) weightsOf(item, out);
  } else if (typeof node === "object" && node !== null && !(node instanceof NamNum)) {
    const keys = Object.keys(node).sort((a, b) => Buffer.compare(Buffer.from(a), Buffer.from(b)));
    for (const key of keys) {
      const val = node[key];
      if (key === "weights" && Array.isArray(val) && val.every((x) => x instanceof NamNum)) {
        out.push(val.map((x) => x.toNumber()));
      } else {
        weightsOf(val, out);
      }
    }
  }
  return out;
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
    } else if (a === "--f16") {
      options.dtype = DTYPE_F16;
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
  // float16 is lossy, so it is asked for by the flag, and the name says it: --f16 writes only a .nam16z,
  // and a .nam16z is only ever written with --f16. The name never picks the dtype by itself.
  const f16 = options.dtype === DTYPE_F16;
  if (f16 !== namedF16(outPath)) {
    process.stderr.write(
      f16
        ? `--f16 writes lossy float16 weights: name the output .nam16z, not ${outPath}\n`
        : `${outPath} is named .nam16z: pass --f16 to write float16 weights (lossy)\n`,
    );
    return 2;
  }
  const src = read(inPath);
  if (src === null) {
    process.stderr.write(`cannot read ${inPath}\n`);
    return 1;
  }
  const packed = pack(src, options);
  if (packed.length === 0) {
    process.stderr.write(
      f16
        ? `encode failed (not valid NAM JSON, or a weight past float16's range: |w| >= 65520, rounds past 65504?): ${inPath}\n`
        : `encode failed (not valid NAM JSON?): ${inPath}\n`,
    );
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
    if (src.length > 6 && src[6] === DTYPE_F16) process.stderr.write("(float16 weights — lossy)\n");
  }
  return 0;
}

function doVerify(argv) {
  if (argv.length < 3) return usage();
  const inPath = argv[2];
  const options = {};
  for (let i = 3; i < argv.length; i++) {
    if (argv[i] === "--f16") {
      options.dtype = DTYPE_F16;
    } else {
      process.stderr.write(`unknown option: ${argv[i]}\n`);
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
    process.stderr.write(`FAIL pack: ${inPath}\n`);
    return 1;
  }
  const back = unpack(packed, DEFAULT_MAX_JSON_BYTES);
  if (back.length === 0) {
    process.stderr.write(`FAIL unpack: ${inPath}\n`);
    return 1;
  }
  const packed2 = pack(back, options);
  const idempotent = packed.length === packed2.length && packed.every((b, i) => b === packed2[i]);
  if (options.dtype === DTYPE_F16) {
    // What float16 cost: the largest change of any weight against the float32 pack, in absolute terms.
    const p32 = pack(src);
    const u32 = p32.length ? unpack(p32, DEFAULT_MAX_JSON_BYTES) : p32;
    const w32 = u32.length ? weightsOf(parse(Buffer.from(u32).toString("utf8")), []) : null;
    const w16 = weightsOf(parse(Buffer.from(back).toString("utf8")), []);
    if (w32 === null || w32.length !== w16.length) {
      process.stderr.write(`FAIL decode: ${inPath}\n`);
      return 1;
    }
    let worst = 0;
    for (let s = 0; s < w32.length; s++)
      for (let k = 0; k < w32[s].length; k++) worst = Math.max(worst, Math.abs(w16[s][k] - w32[s][k]));
    process.stderr.write(
      `${inPath}: ${idempotent ? "OK" : "MISMATCH"}  raw=${src.length} namz=${p32.length} nam16z=${packed.length} (${pct(packed.length, src.length).toFixed(1)}%)  idempotent=${idempotent ? "yes" : "no"}  max|w16-w32|=${formatG3(worst)}\n`,
    );
    return idempotent ? 0 : 1;
  }
  process.stderr.write(
    `${inPath}: ${idempotent ? "OK" : "MISMATCH"}  raw=${src.length} namz=${packed.length} (${pct(packed.length, src.length).toFixed(1)}%)  idempotent=${idempotent ? "yes" : "no"}\n`,
  );
  return idempotent ? 0 : 1;
}

export { formatG3 }; // for its test

export function main(argv) {
  if (argv.length < 2) return usage();
  const cmd = argv[1];
  if (cmd === "encode") return doEncode(argv);
  if (cmd === "decode") return doDecode(argv);
  if (cmd === "map") return doMap(argv);
  if (cmd === "verify") return doVerify(argv);
  return usage();
}
