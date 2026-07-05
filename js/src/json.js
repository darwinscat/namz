// SPDX-License-Identifier: MIT
// A JSON parse/serialize pair tuned to be BYTE-IDENTICAL with the C++ reference (nlohmann/json).
//
// Why not JSON.parse / JSON.stringify? Two reasons:
//   1. JavaScript has a single Number type, so `-18.0` round-trips through JSON.parse as `-18` and
//      JSON.stringify emits `"-18"` — but nlohmann preserves the float and emits `"-18.0"`. We therefore
//      track, per number, whether the source token was an integer or a float, and re-emit accordingly.
//   2. JSON.stringify does not sort object keys; nlohmann (std::map) always emits them sorted by
//      unsigned-byte (== UTF-8) order. We sort keys that way here.
// String escaping is nearly identical between JSON.stringify and nlohmann (both escape only " \ and the
// C0 control chars, and emit non-ASCII raw), so we reuse JSON.stringify — except that since ES2019 it also
// escapes U+2028/U+2029, which nlohmann emits raw; escapeString() below undoes that.

const INT64_MIN = -(2n ** 63n);
const UINT64_MAX = 2n ** 64n - 1n;

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
class Parser {
  constructor(text) {
    this.s = text;
    this.i = 0;
    this.n = text.length;
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
    this.i++; // {
    const obj = Object.create(null);
    this.ws();
    if (this.s[this.i] === "}") {
      this.i++;
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
        return obj;
      }
      this.error("expected ',' or '}'");
    }
  }
  array() {
    this.i++; // [
    const arr = [];
    this.ws();
    if (this.s[this.i] === "]") {
      this.i++;
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
        return JSON.parse(this.s.slice(start, this.i));
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

export function parse(text) {
  return new Parser(text).parse();
}

// ---- serializer ----------------------------------------------------------------------------------
/**
 * Format a double exactly the way nlohmann's dump() does. It emits the shortest round-trip digits
 * (which JS's toExponential() also produces), then chooses fixed vs scientific by the position of the
 * decimal point `n = E + 1`: fixed when `-3 <= n <= 15`, else scientific with a `e±XX` exponent
 * (sign always, at least two digits, zero-padded) — thresholds verified against the reference binary.
 *
 * Note: for float magnitudes beyond ~1e21 nlohmann's own number *parser* is not correctly rounded, so
 * the reconstructed double (and thus this string) can differ by one ULP. Such values never occur in NAM
 * config (dimensions and simple decimals); see the README byte-match note.
 */
export function formatFloat(x) {
  if (Object.is(x, -0)) return "-0.0";
  if (x === 0) return "0.0";
  const neg = x < 0;
  const m = /^(\d)(?:\.(\d+))?e([+-]\d+)$/.exec(Math.abs(x).toExponential());
  const digits = m[1] + (m[2] || ""); // shortest significant digits
  const E = parseInt(m[3], 10); // scientific exponent (value = digits[0].digits[1..] × 10^E)
  const k = digits.length;
  const n = E + 1; // number of digits to the left of the decimal point
  let out;
  if (n >= -3 && n <= 15) {
    if (k <= n) out = digits + "0".repeat(n - k) + ".0"; // 12300.0
    else if (n > 0) out = digits.slice(0, n) + "." + digits.slice(n); // 12.34
    else out = "0." + "0".repeat(-n) + digits; // 0.0034
  } else {
    const mant = k === 1 ? digits : digits[0] + "." + digits.slice(1);
    let ed = Math.abs(E).toString();
    if (ed.length < 2) ed = "0" + ed;
    out = `${mant}e${E < 0 ? "-" : "+"}${ed}`;
  }
  return neg ? "-" + out : out;
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
