// SPDX-License-Identifier: MIT
// A JSON parse/serialize pair tuned to be BYTE-IDENTICAL with the C++ reference (nlohmann/json).
//
// Foundation's JSONSerialization/Codable can't reproduce the reference bytes: they don't preserve the
// integer-vs-float distinction the reference relies on (`-18.0` must stay `-18.0`), don't sort keys by
// UTF-8 byte order, and format doubles differently. So we carry our own tiny JSON model:
//   * integers are kept as their exact decimal STRING (JSON forbids leading zeros, so it is canonical)
//     as long as they fit [Int64.min, UInt64.max]; larger magnitudes are coerced to Double, as nlohmann;
//   * doubles are formatted exactly like nlohmann's dump() (fixed vs scientific thresholds, e±XX).

import Foundation

/// A parsed JSON value that remembers whether each number was an integer or a float.
///
/// Objects are ordered `(key, value)` pairs — NOT a `[String: JSONValue]` — because Swift's `String`
/// equality is Unicode-canonical, which would collapse byte-distinct-but-canonically-equivalent keys
/// (e.g. NFC "é" vs NFD "é"); nlohmann's `std::map` keeps such keys distinct. The parser dedupes by
/// UTF-8 bytes (last write wins) and the serializer sorts by UTF-8 bytes.
indirect enum JSONValue {
    case object([(String, JSONValue)])
    case array([JSONValue])
    case string(String)
    case int(String)  // canonical decimal digits (optionally sign-prefixed), within int64/uint64 range
    case double(Double)
    case bool(Bool)
    case null
}

enum JSONError: Error { case syntax(String) }

// MARK: - Parser

struct JSONParser {
    private let s: [UInt8]
    private var i = 0

    init(_ bytes: [UInt8]) { self.s = bytes }
    init(_ string: String) { self.s = Array(string.utf8) }

    static func parse(_ bytes: [UInt8]) throws -> JSONValue {
        var p = JSONParser(bytes)
        p.skipWS()
        let v = try p.parseValue()
        p.skipWS()
        if p.i != p.s.count { throw JSONError.syntax("trailing content") }
        return v
    }

    private mutating func skipWS() {
        while i < s.count {
            let c = s[i]
            if c == 0x20 || c == 0x09 || c == 0x0a || c == 0x0d { i += 1 } else { break }
        }
    }

    private mutating func parseValue() throws -> JSONValue {
        skipWS()
        guard i < s.count else { throw JSONError.syntax("unexpected end") }
        switch s[i] {
        case 0x7b: return try parseObject()  // {
        case 0x5b: return try parseArray()   // [
        case 0x22: return .string(try parseString())  // "
        case 0x74: try expect("true"); return .bool(true)
        case 0x66: try expect("false"); return .bool(false)
        case 0x6e: try expect("null"); return .null
        case 0x2d, 0x30...0x39: return try parseNumber()  // - or digit
        default: throw JSONError.syntax("unexpected byte \(s[i])")
        }
    }

    private mutating func expect(_ literal: String) throws {
        for b in literal.utf8 {
            guard i < s.count, s[i] == b else { throw JSONError.syntax("invalid literal") }
            i += 1
        }
    }

    private mutating func parseObject() throws -> JSONValue {
        i += 1  // {
        var pairs: [(String, JSONValue)] = []
        var index: [[UInt8]: Int] = [:]  // byte-key → position (for byte-identical last-wins dedup)
        skipWS()
        if i < s.count, s[i] == 0x7d { i += 1; return .object(pairs) }
        while true {
            skipWS()
            guard i < s.count, s[i] == 0x22 else { throw JSONError.syntax("expected key") }
            let key = try parseString()
            skipWS()
            guard i < s.count, s[i] == 0x3a else { throw JSONError.syntax("expected ':'") }
            i += 1
            let val = try parseValue()
            let kb = Array(key.utf8)
            if let pos = index[kb] { pairs[pos].1 = val }  // duplicate byte-identical key: last wins
            else { index[kb] = pairs.count; pairs.append((key, val)) }
            skipWS()
            guard i < s.count else { throw JSONError.syntax("unterminated object") }
            if s[i] == 0x2c { i += 1; continue }
            if s[i] == 0x7d { i += 1; return .object(pairs) }
            throw JSONError.syntax("expected ',' or '}'")
        }
    }

    private mutating func parseArray() throws -> JSONValue {
        i += 1  // [
        var arr: [JSONValue] = []
        skipWS()
        if i < s.count, s[i] == 0x5d { i += 1; return .array(arr) }
        while true {
            arr.append(try parseValue())
            skipWS()
            guard i < s.count else { throw JSONError.syntax("unterminated array") }
            if s[i] == 0x2c { i += 1; continue }
            if s[i] == 0x5d { i += 1; return .array(arr) }
            throw JSONError.syntax("expected ',' or ']'")
        }
    }

    private mutating func parseString() throws -> String {
        i += 1  // opening quote
        var out: [UInt8] = []
        while i < s.count {
            let c = s[i]
            if c == 0x22 {
                i += 1
                // nlohmann rejects ill-formed UTF-8; String(bytes:encoding:) returns nil on invalid input
                // (String(decoding:) would instead insert U+FFFD, silently accepting it).
                guard let str = String(bytes: out, encoding: .utf8) else {
                    throw JSONError.syntax("invalid UTF-8 in string")
                }
                return str
            }
            if c == 0x5c {  // backslash
                i += 1
                guard i < s.count else { break }
                switch s[i] {
                case 0x22: out.append(0x22)
                case 0x5c: out.append(0x5c)
                case 0x2f: out.append(0x2f)
                case 0x62: out.append(0x08)
                case 0x66: out.append(0x0c)
                case 0x6e: out.append(0x0a)
                case 0x72: out.append(0x0d)
                case 0x74: out.append(0x09)
                case 0x75: try appendUnicodeEscape(&out)
                default: throw JSONError.syntax("bad escape")
                }
                i += 1
            } else if c < 0x20 {
                throw JSONError.syntax("control char in string")  // nlohmann rejects raw controls
            } else {
                out.append(c)
                i += 1
            }
        }
        throw JSONError.syntax("unterminated string")
    }

    private mutating func hex4() throws -> Int {
        guard i + 4 < s.count else { throw JSONError.syntax("bad \\u") }
        var v = 0
        for _ in 0..<4 {
            i += 1
            let c = s[i]
            let d: Int
            switch c {
            case 0x30...0x39: d = Int(c - 0x30)
            case 0x41...0x46: d = Int(c - 0x41 + 10)
            case 0x61...0x66: d = Int(c - 0x61 + 10)
            default: throw JSONError.syntax("bad hex")
            }
            v = v * 16 + d
        }
        return v
    }

    private mutating func appendUnicodeEscape(_ out: inout [UInt8]) throws {
        var scalar = try hex4()
        if scalar >= 0xD800 && scalar <= 0xDBFF {  // high surrogate → expect a low surrogate
            guard i + 2 < s.count, s[i + 1] == 0x5c, s[i + 2] == 0x75 else {
                throw JSONError.syntax("lone high surrogate")
            }
            i += 2  // consume "\u"
            let low = try hex4()
            guard low >= 0xDC00 && low <= 0xDFFF else { throw JSONError.syntax("bad low surrogate") }
            scalar = 0x10000 + ((scalar - 0xD800) << 10) + (low - 0xDC00)
        } else if scalar >= 0xDC00 && scalar <= 0xDFFF {
            throw JSONError.syntax("lone low surrogate")
        }
        appendScalar(UInt32(scalar), &out)
    }

    private func appendScalar(_ scalar: UInt32, _ out: inout [UInt8]) {
        if scalar < 0x80 {
            out.append(UInt8(scalar))
        } else if scalar < 0x800 {
            out.append(UInt8(0xC0 | (scalar >> 6)))
            out.append(UInt8(0x80 | (scalar & 0x3F)))
        } else if scalar < 0x10000 {
            out.append(UInt8(0xE0 | (scalar >> 12)))
            out.append(UInt8(0x80 | ((scalar >> 6) & 0x3F)))
            out.append(UInt8(0x80 | (scalar & 0x3F)))
        } else {
            out.append(UInt8(0xF0 | (scalar >> 18)))
            out.append(UInt8(0x80 | ((scalar >> 12) & 0x3F)))
            out.append(UInt8(0x80 | ((scalar >> 6) & 0x3F)))
            out.append(UInt8(0x80 | (scalar & 0x3F)))
        }
    }

    private mutating func parseNumber() throws -> JSONValue {
        let start = i
        if s[i] == 0x2d { i += 1 }
        // integer part
        guard i < s.count else { throw JSONError.syntax("bad number") }
        if s[i] == 0x30 {
            i += 1
        } else if s[i] >= 0x31 && s[i] <= 0x39 {
            while i < s.count, s[i] >= 0x30 && s[i] <= 0x39 { i += 1 }
        } else {
            throw JSONError.syntax("bad number")
        }
        var isFloat = false
        if i < s.count, s[i] == 0x2e {  // .
            isFloat = true
            i += 1
            guard i < s.count, s[i] >= 0x30 && s[i] <= 0x39 else { throw JSONError.syntax("bad frac") }
            while i < s.count, s[i] >= 0x30 && s[i] <= 0x39 { i += 1 }
        }
        if i < s.count, s[i] == 0x65 || s[i] == 0x45 {  // e/E
            isFloat = true
            i += 1
            if i < s.count, s[i] == 0x2b || s[i] == 0x2d { i += 1 }
            guard i < s.count, s[i] >= 0x30 && s[i] <= 0x39 else { throw JSONError.syntax("bad exp") }
            while i < s.count, s[i] >= 0x30 && s[i] <= 0x39 { i += 1 }
        }
        let token = String(decoding: s[start..<i], as: UTF8.self)
        if isFloat {
            guard let d = Double(token), d.isFinite else { throw JSONError.syntax("non-finite") }
            return .double(d)
        }
        // Integer: keep exact if it fits int64/uint64, else coerce to Double (matches nlohmann). "-0" is
        // a valid JSON integer that nlohmann normalizes to 0 (and (float)0 is +0.0, not -0.0).
        if token.hasPrefix("-") {
            if Int64(token) != nil { return .int(token == "-0" ? "0" : token) }
        } else {
            if UInt64(token) != nil { return .int(token) }
        }
        guard let d = Double(token), d.isFinite else { throw JSONError.syntax("non-finite") }
        return .double(d)
    }
}

// MARK: - Serializer

/// Byte-order (UTF-8) comparison for object keys, matching nlohmann's std::map ordering.
func utf8Less(_ a: String, _ b: String) -> Bool {
    let ba = Array(a.utf8), bb = Array(b.utf8)
    let n = min(ba.count, bb.count)
    var i = 0
    while i < n {
        if ba[i] != bb[i] { return ba[i] < bb[i] }
        i += 1
    }
    return ba.count < bb.count
}

/// Escape a string the way nlohmann does: escape only " \ and the C0 controls; emit everything else
/// (incl. non-ASCII and U+2028/U+2029) raw.
func escapeJSONString(_ str: String) -> String {
    var out = "\""
    for scalar in str.unicodeScalars {
        switch scalar.value {
        case 0x22: out += "\\\""
        case 0x5c: out += "\\\\"
        case 0x08: out += "\\b"
        case 0x0c: out += "\\f"
        case 0x0a: out += "\\n"
        case 0x0d: out += "\\r"
        case 0x09: out += "\\t"
        case 0..<0x20:
            out += String(format: "\\u%04x", scalar.value)
        default:
            out.unicodeScalars.append(scalar)
        }
    }
    out += "\""
    return out
}

/// Format a Double exactly like nlohmann's dump(): shortest round-trip digits, fixed when the decimal
/// point position n = E+1 is in [-3, 15], else scientific `d.ddde±XX` (sign always, exponent ≥2 digits).
func formatDouble(_ x: Double) -> String {
    if x == 0 { return x.sign == .minus ? "-0.0" : "0.0" }
    let neg = x < 0
    let (digits, E) = shortestDigits(abs(x))
    let k = digits.count
    let n = E + 1
    var out: String
    if n >= -3 && n <= 15 {
        if k <= n {
            out = digits + String(repeating: "0", count: n - k) + ".0"
        } else if n > 0 {
            let idx = digits.index(digits.startIndex, offsetBy: n)
            out = String(digits[..<idx]) + "." + String(digits[idx...])
        } else {
            out = "0." + String(repeating: "0", count: -n) + digits
        }
    } else {
        let mant: String
        if k == 1 {
            mant = digits
        } else {
            let idx = digits.index(digits.startIndex, offsetBy: 1)
            mant = String(digits[..<idx]) + "." + String(digits[idx...])
        }
        var ed = String(abs(E))
        if ed.count < 2 { ed = "0" + ed }
        out = mant + "e" + (E < 0 ? "-" : "+") + ed
    }
    return neg ? "-" + out : out
}

/// Shortest significant digits + exponent of a positive finite Double, from Swift's own shortest
/// description (normalized to `digits × 10^E` with leading digit at position E).
private func shortestDigits(_ d: Double) -> (String, Int) {
    let desc = String(d)  // Swift prints the shortest round-trip representation
    var mantissa = desc
    var exp10 = 0
    if let eIdx = desc.firstIndex(where: { $0 == "e" || $0 == "E" }) {
        mantissa = String(desc[desc.startIndex..<eIdx])
        exp10 = Int(desc[desc.index(after: eIdx)...]) ?? 0
    }
    var intPart = mantissa
    var fracPart = ""
    if let dot = mantissa.firstIndex(of: ".") {
        intPart = String(mantissa[mantissa.startIndex..<dot])
        fracPart = String(mantissa[mantissa.index(after: dot)...])
    }
    let allDigits = Array((intPart + fracPart).utf8)
    let intCount = intPart.count
    // first and last non-zero
    var f = 0
    while f < allDigits.count, allDigits[f] == 0x30 { f += 1 }
    var l = allDigits.count - 1
    while l > f, allDigits[l] == 0x30 { l -= 1 }
    if f >= allDigits.count { return ("0", 0) }  // all zeros (shouldn't happen for d != 0)
    let sig = String(decoding: allDigits[f...l], as: UTF8.self)
    let E = intCount - 1 - f + exp10
    return (sig, E)
}

func serialize(_ node: JSONValue) -> String {
    switch node {
    case .null: return "null"
    case .bool(let b): return b ? "true" : "false"
    case .int(let s): return s
    case .double(let d): return formatDouble(d)
    case .string(let s): return escapeJSONString(s)
    case .array(let arr):
        return "[" + arr.map(serialize).joined(separator: ",") + "]"
    case .object(let pairs):
        let sorted = pairs.sorted { utf8Less($0.0, $1.0) }
        return "{" + sorted.map { escapeJSONString($0.0) + ":" + serialize($0.1) }.joined(separator: ",") + "}"
    }
}
