// SPDX-License-Identifier: MIT
import Foundation
import Namz

// MARK: Unit — public API behaviour

func runUnit(_ r: Reporter) {
    r.check(Namz.isNamz(s2b("NAMZ....")), "isNamz true")
    r.check(!Namz.isNamz(s2b("XAMZ")), "isNamz false")
    r.check(!Namz.isNamz([]), "isNamz empty")

    r.eq(b2s(Namz.unpack(Namz.pack(s2b("{\"b\":2,\"architecture\":\"X\",\"weights\":[1.0,2.0],\"a\":1}")))),
         "{\"a\":1,\"architecture\":\"X\",\"b\":2,\"weights\":[1.0,2.0]}", "minify+sort")

    r.eq(b2s(Namz.unpack(Namz.pack(s2b("{\"loudness\":-18.0,\"weights\":[1.0]}")))),
         "{\"loudness\":-18.0,\"weights\":[1.0]}", "int-valued float keeps decimal")

    r.eq(b2s(Namz.unpack(Namz.pack(s2b("{\"architecture\":\"X\",\"weights\":[]}")))),
         "{\"architecture\":\"X\",\"weights\":[]}", "empty weights preserved")

    r.check(b2s(Namz.unpack(Namz.pack(s2b("{\"x\":18446744073709551616,\"weights\":[1.0]}"))))
        .contains("\"x\":1.8446744073709552e+19"), "2^64 coerced to double")
    let bigints = b2s(Namz.unpack(Namz.pack(
        s2b("{\"a\":18446744073709551615,\"b\":-9223372036854775808,\"weights\":[1.0]}"))))
    r.check(bigints.contains("\"a\":18446744073709551615"), "uint64 max kept")
    r.check(bigints.contains("\"b\":-9223372036854775808"), "int64 min kept")

    let sep = "\u{2028}y\u{2029}"
    let u = b2s(Namz.unpack(Namz.pack(s2b("{\"s\":\"a\(sep)b\",\"weights\":[1.0]}"))))
    r.check(u.contains("a\(sep)b") && !u.contains("\\u2028"), "U+2028 stays raw")

    let meta = Namz.pack(s2b("{\"architecture\":\"WaveNet\",\"weights\":[0.5]}"),
                         options: PackOptions(metadata: ["tone_type": "hi-gain", "boost": "true",
                                                         "count": "42", "device": "tube:1"]))
    r.eq(Namz.readMeta(meta), ["tone_type": "hi-gain", "boost": "true", "count": "42", "device": "tube:1"],
         "readMeta typed")
    r.check(b2s(Namz.unpack(meta)).contains("\"boost\":true"), "boost typed bool")

    let lz = Namz.pack(s2b("{\"architecture\":\"X\",\"weights\":[1.0]}"),
                       options: PackOptions(metadata: ["count": "000000000000000000001", "zeros": "0000"]))
    r.eq(Namz.readMeta(lz)["count"], "1", "leading-zero metadata → 1")
    r.eq(Namz.readMeta(lz)["zeros"], "0", "all-zeros metadata → 0")
    r.check(b2s(Namz.unpack(lz)).contains("\"count\":1"), "leading-zero int in skeleton")

    r.eq(Namz.readMeta(s2b("not a namz")), [:], "readMeta non-namz empty")
    r.eq(Namz.readMeta([0x4e, 0x41, 0x4d, 0x5a, 1, 0, 0, 0, 0x2e, 0x2e]), [:], "readMeta v1 empty")
    r.eq(Namz.readMeta(Namz.pack(s2b("{\"architecture\":\"X\",\"weights\":[1.0]}"))), [:], "readMeta no-meta empty")

    let nb = Namz.pack(s2b("{\"architecture\":\"X\",\"weights\":[1.0]}"))
    r.check(nb[8] == 0 && nb[9] == 0, "no header block")

    r.check(Namz.pack(s2b("[1,2,3]"), options: PackOptions(metadata: ["foo": "bar"])).isEmpty,
            "metadata on non-object rejected")

    // -0 integer normalizes to 0 (matches nlohmann), including as a weight (+0.0, not -0.0)
    r.eq(b2s(Namz.unpack(Namz.pack(s2b("{\"x\":-0,\"weights\":[1.0]}")))), "{\"weights\":[1.0],\"x\":0}",
         "integer -0 → 0")
    r.eq(b2s(Namz.unpack(Namz.pack(s2b("{\"weights\":[-0]}")))), "{\"weights\":[0.0]}", "-0 weight → +0.0")

    // byte-distinct-but-canonically-equivalent keys must both survive (NFC é vs NFD é)
    let raw = Namz.unpack(Namz.pack(s2b("{\"\u{00e9}\":1,\"e\u{0301}\":2,\"weights\":[1.0]}")))
    func containsSeq(_ hay: [UInt8], _ needle: [UInt8]) -> Bool {
        guard needle.count <= hay.count else { return false }
        for i in 0...(hay.count - needle.count) where Array(hay[i..<i + needle.count]) == needle { return true }
        return false
    }
    r.check(containsSeq(raw, [0xc3, 0xa9]) && containsSeq(raw, [0x65, 0xcc, 0x81]),
            "both NFC and NFD keys preserved (no canonical collapse)")

    // ill-formed UTF-8 in a string must be rejected like nlohmann
    var badUTF8 = s2b("{\"s\":\"")
    badUTF8.append(0xff)
    badUTF8 += s2b("\",\"weights\":[1.0]}")
    r.check(Namz.pack(badUTF8).isEmpty, "invalid UTF-8 rejected")

    // Int.max cap must not trap
    let anyBlob = Namz.pack(s2b("{\"architecture\":\"X\",\"weights\":[0.5]}"))
    r.check(!Namz.unpack(anyBlob, maxJSONBytes: Int.max).isEmpty, "Int.max cap must not trap")
    r.check(Namz.unpack(anyBlob, maxJSONBytes: -1).isEmpty, "negative cap rejected")
}

// MARK: Differential — byte-identical vs the C++ reference CLI (skipped if not built)

func runDifferential(_ r: Reporter) {
    var cli: String?
    if let env = ProcessInfo.processInfo.environment["NAMZ_CLI"], FileManager.default.fileExists(atPath: env) {
        cli = env
    } else {
        var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0..<7 {
            let cand = dir.appendingPathComponent("build/namz")
            if FileManager.default.fileExists(atPath: cand.path) { cli = cand.path; break }
            dir = dir.deletingLastPathComponent()
        }
    }
    guard let cliPath = cli else { print("  (reference CLI not built — differential skipped)"); return }
    let tmp = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("namzswiftdiff-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)

    func refEncode(_ nam: [UInt8], _ shuffle: Bool, _ sets: [String: String]) -> [UInt8]? {
        let inp = tmp.appendingPathComponent("in.nam"), out = tmp.appendingPathComponent("out.namz")
        try? Data(nam).write(to: inp)
        try? FileManager.default.removeItem(at: out)
        var args = ["encode", inp.path, out.path]
        if !shuffle { args.append("--no-shuffle") }
        for (k, v) in sets { args += ["--set", "\(k)=\(v)"] }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: cliPath)
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run(); p.waitUntilExit() } catch { return nil }
        guard p.terminationStatus == 0, let d = try? Data(contentsOf: out) else { return nil }
        return [UInt8](d)
    }
    func check(_ nam: [UInt8], _ shuffle: Bool = true, _ sets: [String: String] = [:]) {
        let ref = refEncode(nam, shuffle, sets)
        let mine = Namz.pack(nam, options: PackOptions(shuffle: shuffle, metadata: sets))
        r.eq(ref == nil, mine.isEmpty, "accept/reject differs: \(b2s(Array(nam.prefix(50))))")
        if let ref { r.eq(ref, mine, "byte mismatch: \(b2s(Array(nam.prefix(70))))") }
    }

    let targeted = [
        "{\"sample_rate\":48000.0,\"weights\":[1.0,2.0]}",
        "{\"loudness\":-18.0,\"x\":0.1,\"y\":0.3333333333333333,\"weights\":[0.5]}",
        "{\"x\":18446744073709551616,\"weights\":[1.0]}",
        "{\"a\":9223372036854775807,\"b\":18446744073709551615,\"weights\":[1.0]}",
        "{\"s\":\"a\\tb\\nc\\u0001\\\"\\\\\",\"weights\":[1.0]}",
        "{\"weights\":[0,1,-1,16777216,-16777216]}",
        "[1,2,3]",
        "{\"config\":{\"z\":1,\"a\":2,\"m\":[{\"weights\":[1.0]},{\"weights\":[]}]},\"weights\":[9.0]}",
        "{\"ключ\":\"значение\",\"emoji\":\"🎸\",\"weights\":[1.0]}",
        "{\"s\":\"a\u{2028}y\u{2029}b\",\"weights\":[1.0]}",
        "{\"x\":-0,\"weights\":[-0]}",  // integer -0 → 0 / +0.0
        "{\"\u{00e9}\":1,\"e\u{0301}\":2,\"weights\":[1.0]}",  // NFC vs NFD keys stay distinct
    ]
    for j in targeted { check(s2b(j), true); check(s2b(j), false) }
    check(s2b("{\"architecture\":\"X\",\"weights\":[0.5]}"), true,
          ["tone_type": "hi-gain", "boost": "true", "n": "42", "device": "tube:1,pnp:1"])
    check(s2b("{\"architecture\":\"X\",\"weights\":[0.5]}"), true, ["count": "000000000000000000001", "z": "0000"])
    check(s2b("[1,2,3]"), true, ["foo": "bar"])

    // random fuzz (in-domain config values → byte-identical)
    var rng = SeededRNG(0xC0FFEE)
    let nice = [0.0, -0.0, 0.5, 0.25, 1.0 / 3.0, 2.5, -18.0, 48000.0, 6.0]
    func ri(_ lo: Int, _ hi: Int) -> Int { lo + Int(rng.next() % UInt64(hi - lo + 1)) }
    for _ in 0..<250 {
        var fields: [String] = []
        var used = Set<String>()
        for _ in 0..<ri(0, 4) {
            let key = ["architecture", "sample_rate", "gain", "cfg", "zzz", "aaa"][ri(0, 5)]
            if !used.insert(key).inserted { continue }
            let v: String
            switch rng.next() % 5 {
            case 0: v = String(ri(-100000, 100000))
            case 1: v = Double(nice[ri(0, nice.count - 1)]).description
            case 2: v = "\"" + ["WaveNet", "LSTM", "X"][ri(0, 2)] + "\""
            case 3: v = ["true", "false", "null"][ri(0, 2)]
            default: v = "null"
            }
            fields.append("\"\(key)\":\(v)")
        }
        if rng.next() % 10 < 7 {
            var ws: [String] = []
            for _ in 0..<ri(0, 40) {
                let f = Float(bitPattern: UInt32(truncatingIfNeeded: rng.next()))
                ws.append(f.isFinite ? Double(f).description : "0.0")
            }
            fields.append("\"weights\":[\(ws.joined(separator: ","))]")
        }
        check(s2b("{" + fields.joined(separator: ",") + "}"), rng.next() % 2 == 0)
    }
    try? FileManager.default.removeItem(at: tmp)
}
