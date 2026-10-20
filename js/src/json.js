// SPDX-License-Identifier: MIT
// A JSON parse/serialize pair tuned to be BYTE-IDENTICAL with the C++ reference (nlohmann/json 3.11).
//
// Why not JSON.parse / JSON.stringify? Four reasons:
//   1. JavaScript has a single Number type, so `-18.0` round-trips through JSON.parse as `-18` and
//      JSON.stringify emits `"-18"` — but nlohmann preserves the float and emits `"-18.0"`. We therefore
//      track, per number, whether the source token was an integer or a float, and re-emit accordingly.
//   2. JSON.stringify does not sort object keys; nlohmann (std::map) always emits them sorted by
//      unsigned-byte (== UTF-8) order. We sort keys that way here.
//   3. nlohmann picks a double's digits with Grisu2, which is neither always the shortest nor always the
//      closest: -198.70986938476562 prints as -198.70986938476563. formatFloat() is a port of its dtoa_impl.
//   4. JSON.parse takes an escaped lone surrogate ("\ud800"); nlohmann refuses the text.
// String escaping is nearly identical between JSON.stringify and nlohmann (both escape only " \ and the
// C0 control chars, and emit non-ASCII raw), so we reuse JSON.stringify — except that since ES2019 it also
// escapes U+2028/U+2029, which nlohmann emits raw; escapeString() below undoes that.

/**
 * The deepest any JSON in a `.namz` may nest (NAMZ-FORMAT.md "Placeholders and depth", the reference's
 * `namz::kMaxDepth`): a scalar is 0, an array or object is 1 + the deepest of its members, so an empty one is
 * 1. Every parse below refuses anything deeper, so no walk over a parsed tree — extract, refill, serialize,
 * a caller's own — ever recurses past it.
 */
export const MAX_DEPTH = 128;

const INT64_MIN = -(2n ** 63n);
const UINT64_MAX = 2n ** 64n - 1n;

// A UTF-16 code unit of a surrogate pair standing alone: no Unicode text, so no JSON string nlohmann reads.
const LONE_SURROGATE = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/;

/** A parsed JSON number that remembers whether it was written as an integer or a float. */
export class NamNum {
  constructor(isFloat, value) {
    this.isFloat = isFloat; // true → double (stored in `num`); false → integer (stored in `int`)
    if (isFloat) {
      this.num = value; // JS number (double)
    } else {
      this.int = value; // BigInt (exact)
    }
  }
  static int(bigintValue) {
    return new NamNum(false, bigintValue);
  }
  static float(numberValue) {
    return new NamNum(true, numberValue);
  }
  /** Numeric value as a JS number (for float32 conversion of weights). */
  toNumber() {
    return this.isFloat ? this.num : Number(this.int);
  }
}

const _enc = new TextEncoder();

/** Compare two strings by their UTF-8 byte sequences (== nlohmann std::map key order). */
function compareUtf8(a, b) {
  if (a === b) return 0;
  const ba = _enc.encode(a);
  const bb = _enc.encode(b);
  const n = Math.min(ba.length, bb.length);
  for (let i = 0; i < n; i++) {
    if (ba[i] !== bb[i]) return ba[i] - bb[i];
  }
  return ba.length - bb.length;
}

// ---- parser (recursive descent) ------------------------------------------------------------------
// It counts the containers it is inside and refuses the one that would make them more than `maxDepth`
// BEFORE descending into it, so the depth is measured by a counter, never by a walk, and the parser's own
// recursion stops at `maxDepth` frames however deep the text goes.
class Parser {
  constructor(text, maxDepth) {
    this.s = text;
    this.i = 0;
    this.n = text.length;
    this.depth = 0; // containers open around the current position
    this.maxDepth = maxDepth;
  }
  enter() {
    this.i++; // [ or {
    if (++this.depth > this.maxDepth) this.error(`nested deeper than ${this.maxDepth}`);
  }
  error(msg) {
    throw new SyntaxError(`namz JSON parse: ${msg} at ${this.i}`);
  }
  ws() {
    while (this.i < this.n) {
      const c = this.s.charCodeAt(this.i);
      if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) this.i++;
      else break;
    }
  }
  parse() {
    this.ws();
    const v = this.value();
    this.ws();
    if (this.i !== this.n) this.error("trailing content");
    return v;
  }
  value() {
    this.ws();
    if (this.i >= this.n) this.error("unexpected end");
    const c = this.s[this.i];
    if (c === "{") return this.object();
    if (c === "[") return this.array();
    if (c === '"') return this.string();
    if (c === "t" || c === "f") return this.bool();
    if (c === "n") return this.null_();
    if (c === "-" || (c >= "0" && c <= "9")) return this.number();
    this.error(`unexpected character ${JSON.stringify(c)}`);
  }
  object() {
    this.enter();
    const obj = Object.create(null);
    this.ws();
    if (this.s[this.i] === "}") {
      this.i++;
      this.depth--;
      return obj;
    }
    for (;;) {
      this.ws();
      if (this.s[this.i] !== '"') this.error("expected key string");
      const key = this.string();
      this.ws();
      if (this.s[this.i] !== ":") this.error("expected ':'");
      this.i++;
      obj[key] = this.value();
      this.ws();
      const ch = this.s[this.i];
      if (ch === ",") {
        this.i++;
        continue;
      }
      if (ch === "}") {
        this.i++;
        this.depth--;
        return obj;
      }
      this.error("expected ',' or '}'");
    }
  }
  array() {
    this.enter();
    const arr = [];
    this.ws();
    if (this.s[this.i] === "]") {
      this.i++;
      this.depth--;
      return arr;
    }
    for (;;) {
      arr.push(this.value());
      this.ws();
      const ch = this.s[this.i];
      if (ch === ",") {
        this.i++;
        continue;
      }
      if (ch === "]") {
        this.i++;
        this.depth--;
        return arr;
      }
      this.error("expected ',' or ']'");
    }
  }
  string() {
    // Delegate to JSON.parse for a single string token (correct unescaping incl. surrogate pairs).
    const start = this.i;
    this.i++; // opening quote
    while (this.i < this.n) {
      const c = this.s[this.i];
      if (c === "\\") {
        this.i += 2;
        continue;
      }
      if (c === '"') {
        this.i++;
        const v = JSON.parse(this.s.slice(start, this.i));
        if (LONE_SURROGATE.test(v)) this.error("lone surrogate");
        return v;
      }
      this.i++;
    }
    this.error("unterminated string");
  }
  bool() {
    if (this.s.startsWith("true", this.i)) {
      this.i += 4;
      return true;
    }
    if (this.s.startsWith("false", this.i)) {
      this.i += 5;
      return false;
    }
    this.error("invalid literal");
  }
  null_() {
    if (this.s.startsWith("null", this.i)) {
      this.i += 4;
      return null;
    }
    this.error("invalid literal");
  }
  number() {
    const start = this.i;
    if (this.s[this.i] === "-") this.i++;
    // integer part
    if (this.s[this.i] === "0") {
      this.i++;
    } else if (this.s[this.i] >= "1" && this.s[this.i] <= "9") {
      while (this.s[this.i] >= "0" && this.s[this.i] <= "9") this.i++;
    } else {
      this.error("invalid number");
    }
    let isFloat = false;
    if (this.s[this.i] === ".") {
      isFloat = true;
      this.i++;
      if (!(this.s[this.i] >= "0" && this.s[this.i] <= "9")) this.error("invalid fraction");
      while (this.s[this.i] >= "0" && this.s[this.i] <= "9") this.i++;
    }
    if (this.s[this.i] === "e" || this.s[this.i] === "E") {
      isFloat = true;
      this.i++;
      if (this.s[this.i] === "+" || this.s[this.i] === "-") this.i++;
      if (!(this.s[this.i] >= "0" && this.s[this.i] <= "9")) this.error("invalid exponent");
      while (this.s[this.i] >= "0" && this.s[this.i] <= "9") this.i++;
    }
    const token = this.s.slice(start, this.i);
    if (isFloat) {
      const v = Number(token);
      if (!Number.isFinite(v)) this.error("non-finite number"); // out of contract (matches reference)
      return NamNum.float(v);
    }
    const big = BigInt(token);
    // nlohmann stores an integer literal outside [int64_min, uint64_max] as a double.
    if (big < INT64_MIN || big > UINT64_MAX) {
      const v = Number(token);
      if (!Number.isFinite(v)) this.error("non-finite number");
      return NamNum.float(v);
    }
    return NamNum.int(big);
  }
}

/** Parse JSON text, refusing (SyntaxError) anything nested deeper than `maxDepth`. */
export function parse(text, maxDepth = MAX_DEPTH) {
  return new Parser(text, maxDepth).parse();
}

// ---- serializer ----------------------------------------------------------------------------------
// nlohmann's dtoa_impl (to_chars.hpp, after Florian Loitsch's Grisu2), step for step, with BigInt standing in
// for its uint64_t. No step of it ever needs more than 64 bits, so nothing here grows where C++ would wrap.
//
// Cached powers of ten c = f * 2^e ~= 10^k, k = -300 + 8 i: nlohmann's table, verbatim [f, e].
const CACHED_POWERS = [
  [0xAB70FE17C79AC6CAn, -1060], [0xFF77B1FCBEBCDC4Fn, -1034], [0xBE5691EF416BD60Cn, -1007],
  [0x8DD01FAD907FFC3Cn, -980], [0xD3515C2831559A83n, -954], [0x9D71AC8FADA6C9B5n, -927], [0xEA9C227723EE8BCBn, -901],
  [0xAECC49914078536Dn, -874], [0x823C12795DB6CE57n, -847], [0xC21094364DFB5637n, -821], [0x9096EA6F3848984Fn, -794],
  [0xD77485CB25823AC7n, -768], [0xA086CFCD97BF97F4n, -741], [0xEF340A98172AACE5n, -715], [0xB23867FB2A35B28En, -688],
  [0x84C8D4DFD2C63F3Bn, -661], [0xC5DD44271AD3CDBAn, -635], [0x936B9FCEBB25C996n, -608], [0xDBAC6C247D62A584n, -582],
  [0xA3AB66580D5FDAF6n, -555], [0xF3E2F893DEC3F126n, -529], [0xB5B5ADA8AAFF80B8n, -502], [0x87625F056C7C4A8Bn, -475],
  [0xC9BCFF6034C13053n, -449], [0x964E858C91BA2655n, -422], [0xDFF9772470297EBDn, -396], [0xA6DFBD9FB8E5B88Fn, -369],
  [0xF8A95FCF88747D94n, -343], [0xB94470938FA89BCFn, -316], [0x8A08F0F8BF0F156Bn, -289], [0xCDB02555653131B6n, -263],
  [0x993FE2C6D07B7FACn, -236], [0xE45C10C42A2B3B06n, -210], [0xAA242499697392D3n, -183], [0xFD87B5F28300CA0En, -157],
  [0xBCE5086492111AEBn, -130], [0x8CBCCC096F5088CCn, -103], [0xD1B71758E219652Cn, -77], [0x9C40000000000000n, -50],
  [0xE8D4A51000000000n, -24], [0xAD78EBC5AC620000n, 3], [0x813F3978F8940984n, 30], [0xC097CE7BC90715B3n, 56],
  [0x8F7E32CE7BEA5C70n, 83], [0xD5D238A4ABE98068n, 109], [0x9F4F2726179A2245n, 136], [0xED63A231D4C4FB27n, 162],
  [0xB0DE65388CC8ADA8n, 189], [0x83C7088E1AAB65DBn, 216], [0xC45D1DF942711D9An, 242], [0x924D692CA61BE758n, 269],
  [0xDA01EE641A708DEAn, 295], [0xA26DA3999AEF774An, 322], [0xF209787BB47D6B85n, 348], [0xB454E4A179DD1877n, 375],
  [0x865B86925B9BC5C2n, 402], [0xC83553C5C8965D3Dn, 428], [0x952AB45CFA97A0B3n, 455], [0xDE469FBD99A05FE3n, 481],
  [0xA59BC234DB398C25n, 508], [0xF6C69A72A3989F5Cn, 534], [0xB7DCBF5354E9BECEn, 561], [0x88FCF317F22241E2n, 588],
  [0xCC20CE9BD35C78A5n, 614], [0x98165AF37B2153DFn, 641], [0xE2A0B5DC971F303An, 667], [0xA8D9D1535CE3B396n, 694],
  [0xFB9B7CD9A4A7443Cn, 720], [0xBB764C4CA7A44410n, 747], [0x8BAB8EEFB6409C1An, 774], [0xD01FEF10A657842Cn, 800],
  [0x9B10A4E5E9913129n, 827], [0xE7109BFBA19C0C9Dn, 853], [0xAC2820D9623BF429n, 880], [0x80444B5E7AA7CF85n, 907],
  [0xBF21E44003ACDD2Dn, 933], [0x8E679C2F5E44FF8Fn, 960], [0xD433179D9C8CB841n, 986], [0x9E19DB92B4E31BA9n, 1013],
];
const _f64 = new Float64Array(1);
const _u64 = new BigUint64Array(_f64.buffer); // the same 8 bytes, so the same bits in either byte order
const HALF = 1n << 63n;

/** Bits of the double `x` as a BigInt. */
function bitsOf(x) {
  _f64[0] = x;
  return _u64[0];
}

/** Bit length of a BigInt below 2^64. */
function bitLength(x) {
  const hi = Number(x >> 32n);
  return hi ? 64 - Math.clz32(hi) : 32 - Math.clz32(Number(x));
}

/** Digits and decimal exponent of the finite double > 0 whose bits are `bits`: value ~= digits * 10^exponent. */
function grisu2(bits) {
  const bigE = Number(bits >> 52n);
  const frac = bits & 0xfffffffffffffn;
  let vf = bigE ? frac | 0x10000000000000n : frac;
  const ve = bigE ? bigE - 1075 : -1074;
  // compute_boundaries: m+ and m- halfway to the neighbours, normalized to a 64-bit significand; v, m- and m+
  // all land on m+'s exponent
  let mpf = 2n * vf + 1n;
  const closer = frac === 0n && bigE > 1; // the lower neighbour is closer
  let mmf = closer ? 4n * vf - 1n : 2n * vf - 1n;
  const mme = closer ? ve - 2 : ve - 1;
  const shift = 64 - bitLength(mpf);
  mpf <<= BigInt(shift);
  const e = ve - 1 - shift;
  mmf <<= BigInt(mme - e);
  vf <<= BigInt(ve - e);
  // get_cached_power_for_binary_exponent: C++ integer division truncates toward zero
  const f = -61 - e; // kAlpha - e - 1
  const t = f * 78913;
  const k = (t >= 0 ? Math.floor(t / 262144) : -Math.floor(-t / 262144)) + (f > 0 ? 1 : 0);
  const index = Math.floor((307 + k) / 8);
  const [cf, ce] = CACHED_POWERS[index];
  // grisu2: diyfp::mul keeps the upper 64 bits, rounded half up
  const w = (vf * cf + HALF) >> 64n;
  const lo = ((mmf * cf + HALF) >> 64n) + 1n;
  const hi = ((mpf * cf + HALF) >> 64n) - 1n;
  const exponent = 300 - 8 * index; // -cached.k
  const negE = BigInt(-(e + ce + 64));
  const mask = (1n << negE) - 1n;
  // grisu2_digit_gen
  let delta = hi - lo;
  let dist = hi - w;
  let p1 = Number(hi >> negE); // below 2^32: exact as a Number
  let p2 = hi & mask;
  let digits = "";
  let n = p1 ? String(p1).length : 1; // find_largest_pow10
  let pow10 = 10 ** (n - 1);
  while (n > 0) {
    const d = Math.floor(p1 / pow10);
    p1 -= d * pow10;
    digits += d;
    n--;
    const rest = (BigInt(p1) << negE) + p2;
    if (rest <= delta) return [round(digits, dist, delta, rest, BigInt(pow10) << negE), exponent + n];
    pow10 /= 10;
  }
  let m = 0;
  for (;;) {
    p2 *= 10n;
    digits += Number(p2 >> negE);
    p2 &= mask;
    m++;
    delta *= 10n;
    dist *= 10n;
    if (p2 <= delta) break;
  }
  return [round(digits, dist, delta, p2, 1n << negE), exponent - m];
}

/** grisu2_round: step the last digit down toward the value while that stays inside and comes closer. */
function round(digits, dist, delta, rest, tenK) {
  let last = digits.charCodeAt(digits.length - 1);
  while (rest < dist && delta - rest >= tenK && (rest + tenK < dist || dist - rest > rest + tenK - dist)) {
    last--; // never below '0' (nlohmann asserts it)
    rest += tenK;
  }
  return digits.slice(0, -1) + String.fromCharCode(last);
}

/**
 * Format a double exactly the way nlohmann's dump() does: Grisu2's digits, in fixed form while the decimal
 * point sits within 15 digits of the first one (`-4 < n <= 15`, n = digits before the point), else
 * `d.ddde±XX` (sign always, at least two exponent digits). Throws on a non-finite number (nlohmann prints
 * `null`, which no reader takes back as a number).
 */
export function formatFloat(x) {
  if (!Number.isFinite(x)) throw new RangeError("namz JSON: non-finite number");
  let bits = bitsOf(x);
  let sign = "";
  if (bits >> 63n) {
    sign = "-";
    bits &= 0x7fffffffffffffffn;
  }
  if (bits === 0n) return sign + "0.0";
  const [s, exponent] = grisu2(bits);
  const k = s.length;
  const n = k + exponent;
  if (k <= n && n <= 15) return sign + s + "0".repeat(n - k) + ".0"; // 12300.0
  if (0 < n && n <= 15) return sign + s.slice(0, n) + "." + s.slice(n); // 12.34
  if (-4 < n && n <= 0) return sign + "0." + "0".repeat(-n) + s; // 0.0034
  const ex = n - 1;
  const ed = String(Math.abs(ex)).padStart(2, "0");
  return sign + (k === 1 ? s : s[0] + "." + s.slice(1)) + (ex < 0 ? "e-" : "e+") + ed;
}

/**
 * Format a double as C's printf("%f") does — the reference's std::to_string(double) in readMeta: the exact
 * binary value rounded to six decimals, a tie to the even one.
 */
export function formatFixed6(x) {
  let bits = bitsOf(x);
  const sign = bits >> 63n ? "-" : "";
  bits &= 0x7fffffffffffffffn;
  const bigE = Number(bits >> 52n);
  const frac = bits & 0xfffffffffffffn;
  const m = bigE ? frac | 0x10000000000000n : frac;
  const e2 = bigE ? bigE - 1075 : -1074;
  let q; // round(x * 10^6), half to even
  if (e2 >= 0) {
    q = (m << BigInt(e2)) * 1000000n;
  } else {
    const num = m * 1000000n;
    const den = 1n << BigInt(-e2);
    q = num / den;
    const r2 = 2n * (num - q * den);
    if (r2 > den || (r2 === den && (q & 1n) === 1n)) q++;
  }
  const digits = q.toString().padStart(7, "0");
  return sign + digits.slice(0, -6) + "." + digits.slice(-6);
}

/**
 * Escape a string the way nlohmann does. JSON.stringify handles " \ and the C0 controls identically,
 * but since ES2019 it also escapes U+2028/U+2029 (which nlohmann emits raw); undo that so bytes match.
 */
export function escapeString(s) {
  return JSON.stringify(s)
    .replace(/\u2028/g, String.fromCharCode(0x2028))
    .replace(/\u2029/g, String.fromCharCode(0x2029));
}

export function serialize(node) {
  if (node === null) return "null";
  if (node === true) return "true";
  if (node === false) return "false";
  if (node instanceof NamNum) {
    return node.isFloat ? formatFloat(node.num) : node.int.toString();
  }
  if (typeof node === "string") return escapeString(node);
  if (Array.isArray(node)) {
    let out = "[";
    for (let i = 0; i < node.length; i++) {
      if (i) out += ",";
      out += serialize(node[i]);
    }
    return out + "]";
  }
  // object: sort keys by UTF-8 byte order
  const keys = Object.keys(node).sort(compareUtf8);
  let out = "{";
  for (let i = 0; i < keys.length; i++) {
    if (i) out += ",";
    out += escapeString(keys[i]) + ":" + serialize(node[keys[i]]);
  }
  return out + "}";
}
