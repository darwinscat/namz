// SPDX-License-Identifier: MIT
// namz — a tiny lossless codec for NeuralAmpModeler `.nam` files.
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>.
//
// Native Swift port of the C++ reference (`include/namz.h`), byte-for-byte compatible: `pack` reproduces
// the reference `.namz` exactly and `unpack` rehydrates weights bit-exact (float32). Depends only on the
// standard library + Foundation (for Data conveniences). See NAMZ-FORMAT.md and conformance/.

import Foundation

/// Options for ``Namz/pack(_:options:)``.
public struct PackOptions {
    /// Split each float's 4 bytes into 4 contiguous planes (lossless). Free, and helps an outer compressor.
    public var shuffle: Bool
    /// Display fields to set/overwrite in the top-level `metadata` object and mirror into the header block.
    /// Typed like the reference: `"true"`/`"false"` → bool, all-digits → int, else string.
    public var metadata: [String: String]

    public init(shuffle: Bool = true, metadata: [String: String] = [:]) {
        self.shuffle = shuffle
        self.metadata = metadata
    }
}

public enum Namz {
    static let formatVersion: UInt8 = 2  // readers still accept 1 (no meta block)
    static let codecStore: UInt8 = 0
    static let dtypeF32: UInt8 = 0
    static let flagShuffle: UInt8 = 1
    /// Default cap on the reconstructed `.nam` JSON (zip-bomb guard), matching the reference CLI.
    public static let defaultMaxJSONBytes = 256 * 1024 * 1024

    // MARK: Public API ([UInt8])

    /// True if `data` begins with the `.namz` magic. Cheap; safe on any/short buffer.
    public static func isNamz(_ data: [UInt8]) -> Bool {
        return data.count >= 4 && data[0] == 0x4e && data[1] == 0x41 && data[2] == 0x4d && data[3] == 0x5a
    }

    /// Parse NAM JSON (`.nam`) bytes → packed `.namz` bytes. Empty on failure (invalid JSON, non-finite weight).
    public static func pack(_ data: [UInt8], options: PackOptions = PackOptions()) -> [UInt8] {
        guard var tree = try? JSONParser.parse(data) else { return [] }

        var headerBytes: [UInt8] = []
        if !options.metadata.isEmpty {
            // Metadata can only be stamped onto a JSON object; the reference throws (→ empty) otherwise.
            guard case .object(var pairs) = tree else { return [] }
            let metaIdx = pairs.firstIndex { $0.0 == "metadata" }
            // Reuse an existing `metadata` object, else start fresh (the reference overwrites a non-object).
            var metaPairs: [(String, JSONValue)] = []
            if let mi = metaIdx, case .object(let m) = pairs[mi].1 { metaPairs = m }
            var header: [(String, JSONValue)] = []
            for (k, v) in options.metadata {
                let tv = typeMetaValue(v)
                if let pos = metaPairs.firstIndex(where: { $0.0 == k }) { metaPairs[pos].1 = tv }
                else { metaPairs.append((k, tv)) }
                header.append((k, tv))
            }
            if let mi = metaIdx { pairs[mi].1 = .object(metaPairs) } else { pairs.append(("metadata", .object(metaPairs))) }
            tree = .object(pairs)
            headerBytes = Array(serialize(.object(header)).utf8)
            if headerBytes.count > 0xFFFF { headerBytes = [] }  // u16 length; display metadata is tiny
        }

        var arrays: [[Float]] = []
        let skeletonTree = extractWeights(tree, &arrays)
        for a in arrays {  // non-finite weights are out of contract → reject
            for f in a where !f.isFinite { return [] }
        }
        let skeleton = Array(serialize(skeletonTree).utf8)

        var totalFloats = 0
        for a in arrays { totalFloats += a.count }

        var body: [UInt8] = []
        appendU32(&body, UInt32(skeleton.count))
        body += skeleton
        appendU32(&body, UInt32(arrays.count))
        for a in arrays { appendU32(&body, UInt32(a.count)) }

        if options.shuffle && totalFloats > 0 {
            var flat: [Float] = []
            flat.reserveCapacity(totalFloats)
            for a in arrays { flat += a }
            body += shufflePlanes(floatBytesLE(flat), totalFloats)
        } else {
            for a in arrays where !a.isEmpty { body += floatBytesLE(a) }
        }

        var out: [UInt8] = [0x4e, 0x41, 0x4d, 0x5a, formatVersion, codecStore, dtypeF32,
                            options.shuffle ? flagShuffle : 0]
        appendU16(&out, UInt16(headerBytes.count))
        out += headerBytes
        out += body
        return out
    }

    /// Inverse of ``pack(_:options:)``: `.namz` bytes → reconstructed `.nam` JSON bytes. Empty on any
    /// failure (not a `.namz`, unknown codec/dtype, truncation, corruption, over-cap). Never throws.
    public static func unpack(_ data: [UInt8], maxJSONBytes: Int = defaultMaxJSONBytes) -> [UInt8] {
        let n = data.count
        guard isNamz(data), n >= 8 else { return [] }
        let fmt = data[4], codec = data[5], dtype = data[6], flags = data[7]
        guard fmt <= formatVersion, codec == codecStore, dtype == dtypeF32 else { return [] }

        var off = 8
        if fmt >= 2 {
            guard n >= 10 else { return [] }
            off = 10 + Int(getU16(data, 8))
            guard off <= n else { return [] }
        }
        // Saturating cap check — a caller-supplied Int.max must not overflow (unpack never traps).
        guard maxJSONBytes >= 0 else { return [] }
        let rawCap = maxJSONBytes > Int.max - 4096 ? Int.max : maxJSONBytes + 4096
        guard n - off <= rawCap else { return [] }

        var p = off
        var rem = n - off
        guard rem >= 4 else { return [] }
        let skeletonLen = Int(getU32(data, p)); p += 4; rem -= 4
        guard skeletonLen <= rem else { return [] }
        let skeleton = Array(data[p..<p + skeletonLen]); p += skeletonLen; rem -= skeletonLen

        guard rem >= 4 else { return [] }
        let numArrays = Int(getU32(data, p)); p += 4; rem -= 4
        guard numArrays <= rem / 4 else { return [] }

        var lengths: [Int] = []
        var totalFloats = 0
        for _ in 0..<numArrays {
            let ln = Int(getU32(data, p)); p += 4; rem -= 4
            lengths.append(ln)
            totalFloats += ln
        }
        guard rem % 4 == 0, totalFloats == rem / 4 else { return [] }

        let payload = Array(data[p..<p + rem])
        let floatBytes: [UInt8]
        if totalFloats > 0 {
            floatBytes = (flags & flagShuffle) != 0 ? unshufflePlanes(payload, totalFloats) : payload
        } else {
            floatBytes = []
        }

        var flat: [Float] = []
        flat.reserveCapacity(totalFloats)
        for i in 0..<totalFloats {
            let b = UInt32(floatBytes[4 * i]) | (UInt32(floatBytes[4 * i + 1]) << 8)
                | (UInt32(floatBytes[4 * i + 2]) << 16) | (UInt32(floatBytes[4 * i + 3]) << 24)
            let f = Float(bitPattern: b)
            if !f.isFinite { return [] }  // non-finite payload is out of contract → reject
            flat.append(f)
        }

        var segs: [[Float]] = []
        var o = 0
        for ln in lengths { segs.append(Array(flat[o..<o + ln])); o += ln }

        guard let tree = try? JSONParser.parse(skeleton) else { return [] }
        var ok = true
        let filled = refillWeights(tree, segs, &ok)
        guard ok else { return [] }
        let rebuilt = Array(serialize(filled).utf8)
        guard rebuilt.count <= maxJSONBytes else { return [] }
        return rebuilt
    }

    /// Read the display-metadata block WITHOUT touching the weights. Empty for a v1 `.namz`, a non-`.namz`
    /// buffer, or one packed without metadata.
    public static func readMeta(_ data: [UInt8]) -> [String: String] {
        var out: [String: String] = [:]
        guard isNamz(data), data.count >= 10, data[4] >= 2 else { return out }
        let metaLen = Int(getU16(data, 8))
        guard metaLen != 0, 10 + metaLen <= data.count else { return out }
        guard let tree = try? JSONParser.parse(Array(data[10..<10 + metaLen])),
              case .object(let pairs) = tree else { return out }
        for (k, v) in pairs {
            switch v {
            case .string(let s): out[k] = s
            case .bool(let b): out[k] = b ? "true" : "false"
            case .int(let s): out[k] = s
            case .double(let d): out[k] = String(format: "%f", d)  // matches std::to_string(double)
            default: out[k] = serialize(v)
            }
        }
        return out
    }

    // MARK: Data conveniences

    public static func isNamz(_ data: Data) -> Bool { isNamz([UInt8](data)) }
    public static func pack(_ data: Data, options: PackOptions = PackOptions()) -> Data {
        Data(pack([UInt8](data), options: options))
    }
    public static func unpack(_ data: Data, maxJSONBytes: Int = defaultMaxJSONBytes) -> Data {
        Data(unpack([UInt8](data), maxJSONBytes: maxJSONBytes))
    }
    public static func readMeta(_ data: Data) -> [String: String] { readMeta([UInt8](data)) }
}

// MARK: - internals

private func typeMetaValue(_ s: String) -> JSONValue {
    if s == "true" { return .bool(true) }
    if s == "false" { return .bool(false) }
    if !s.isEmpty && s.allSatisfy({ $0 >= "0" && $0 <= "9" }) {
        // Match std::stoll: leading zeros ignored, typed by value not length ("000…001" → 1).
        let stripped = String(s.drop(while: { $0 == "0" }))
        if stripped.isEmpty { return .int("0") }
        if stripped.count <= 19, let v = UInt64(stripped), v <= UInt64(Int64.max) {
            return .int(stripped)
        }
        return .string(s)
    }
    return .string(s)
}

/// Numeric `"weights"` array → [Float]; nil if any element is not a number (bools excluded).
private func numericWeights(_ v: JSONValue) -> [Float]? {
    guard case .array(let arr) = v else { return nil }
    var floats: [Float] = []
    floats.reserveCapacity(arr.count)
    for el in arr {
        switch el {
        case .int(let s):
            guard let d = Double(s) else { return nil }
            floats.append(Float(d))
        case .double(let d):
            floats.append(Float(d))
        default:
            return nil
        }
    }
    return floats
}

/// DFS in sorted key order (== nlohmann std::map): extract numeric `"weights"` arrays as float32 and
/// replace each with its ordinal integer index.
private func extractWeights(_ node: JSONValue, _ out: inout [[Float]]) -> JSONValue {
    switch node {
    case .object(let pairs):
        var newPairs: [(String, JSONValue)] = []
        newPairs.reserveCapacity(pairs.count)
        // Traverse in sorted key order so ordinal indices match nlohmann (std::map).
        for (key, val) in pairs.sorted(by: { utf8Less($0.0, $1.0) }) {
            if key == "weights", let floats = numericWeights(val) {
                out.append(floats)
                newPairs.append((key, .int(String(out.count - 1))))
            } else {
                newPairs.append((key, extractWeights(val, &out)))
            }
        }
        return .object(newPairs)
    case .array(let arr):
        var newArr: [JSONValue] = []
        newArr.reserveCapacity(arr.count)
        for item in arr { newArr.append(extractWeights(item, &out)) }
        return .array(newArr)
    default:
        return node
    }
}

/// DFS inverse: swap each integer `"weights"` placeholder for its float segment; `ok=false` if OOR.
private func refillWeights(_ node: JSONValue, _ segs: [[Float]], _ ok: inout Bool) -> JSONValue {
    switch node {
    case .object(let pairs):
        var newPairs: [(String, JSONValue)] = []
        newPairs.reserveCapacity(pairs.count)
        for (key, val) in pairs {
            if key == "weights", case .int(let s) = val {
                if let idx = Int(s), idx >= 0, idx < segs.count {
                    newPairs.append((key, .array(segs[idx].map { .double(Double($0)) })))
                } else {
                    ok = false
                    newPairs.append((key, val))
                }
            } else {
                newPairs.append((key, refillWeights(val, segs, &ok)))
            }
        }
        return .object(newPairs)
    case .array(let arr):
        var newArr: [JSONValue] = []
        newArr.reserveCapacity(arr.count)
        for item in arr { newArr.append(refillWeights(item, segs, &ok)) }
        return .array(newArr)
    default:
        return node
    }
}

private func floatBytesLE(_ floats: [Float]) -> [UInt8] {
    var out: [UInt8] = []
    out.reserveCapacity(floats.count * 4)
    for f in floats {
        let b = f.bitPattern
        out.append(UInt8(b & 0xff))
        out.append(UInt8((b >> 8) & 0xff))
        out.append(UInt8((b >> 16) & 0xff))
        out.append(UInt8((b >> 24) & 0xff))
    }
    return out
}

private func shufflePlanes(_ src: [UInt8], _ count: Int) -> [UInt8] {
    var out = [UInt8](repeating: 0, count: count * 4)
    for i in 0..<count {
        out[i] = src[4 * i]
        out[count + i] = src[4 * i + 1]
        out[2 * count + i] = src[4 * i + 2]
        out[3 * count + i] = src[4 * i + 3]
    }
    return out
}

private func unshufflePlanes(_ src: [UInt8], _ count: Int) -> [UInt8] {
    var out = [UInt8](repeating: 0, count: count * 4)
    for i in 0..<count {
        out[4 * i] = src[i]
        out[4 * i + 1] = src[count + i]
        out[4 * i + 2] = src[2 * count + i]
        out[4 * i + 3] = src[3 * count + i]
    }
    return out
}

private func appendU16(_ o: inout [UInt8], _ v: UInt16) {
    o.append(UInt8(v & 0xff)); o.append(UInt8((v >> 8) & 0xff))
}
private func appendU32(_ o: inout [UInt8], _ v: UInt32) {
    o.append(UInt8(v & 0xff)); o.append(UInt8((v >> 8) & 0xff))
    o.append(UInt8((v >> 16) & 0xff)); o.append(UInt8((v >> 24) & 0xff))
}
private func getU16(_ d: [UInt8], _ o: Int) -> UInt16 {
    UInt16(d[o]) | (UInt16(d[o + 1]) << 8)
}
private func getU32(_ d: [UInt8], _ o: Int) -> UInt32 {
    UInt32(d[o]) | (UInt32(d[o + 1]) << 8) | (UInt32(d[o + 2]) << 16) | (UInt32(d[o + 3]) << 24)
}
