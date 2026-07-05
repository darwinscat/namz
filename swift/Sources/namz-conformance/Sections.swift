// SPDX-License-Identifier: MIT
import Foundation
import Namz

// MARK: Conformance — the cross-language TCK

func runConformance(_ r: Reporter) {
    guard let dir = conformanceDir() else { print("  (conformance vectors not found — skipped)"); return }
    guard let mData = try? Data(contentsOf: URL(fileURLWithPath: dir + "/manifest.json")),
          let manifest = try? JSONSerialization.jsonObject(with: mData) as? [String: Any] else {
        r.check(false, "manifest unreadable"); return
    }
    for c in manifest["valid"] as! [[String: Any]] {
        let name = c["name"] as! String
        let input = readBytes(dir + "/" + (c["input"] as! String))
        let expected = readBytes(dir + "/" + (c["output"] as! String))
        let opts = PackOptions(shuffle: (c["shuffle"] as? Bool) ?? true,
                               metadata: (c["set"] as? [String: String]) ?? [:])
        r.eq(Namz.pack(input, options: opts), expected, "[\(name)] encode byte mismatch")

        let decoded = Namz.unpack(expected)
        r.check(!decoded.isEmpty, "[\(name)] decode empty")
        var want: [[Float]] = [], got: [[Float]] = []
        if let a = try? JSONSerialization.jsonObject(with: Data(input)) { gatherFloats(a, &want) }
        if let b = try? JSONSerialization.jsonObject(with: Data(decoded)) { gatherFloats(b, &got) }
        r.eq(want.map(floatBits), got.map(floatBits), "[\(name)] weights not bit-exact")
        r.eq(Namz.pack(decoded, options: opts), expected, "[\(name)] not idempotent")
    }
    for c in manifest["invalid"] as! [[String: Any]] {
        let blob = readBytes(dir + "/" + (c["file"] as! String))
        r.check(Namz.unpack(blob).isEmpty, "[\(c["name"]!)] should be rejected")
    }
}

// MARK: Property — random finite float32 round-trip

private func randomFinite(_ rng: inout SeededRNG, _ n: Int) -> [Float] {
    var out: [Float] = []
    while out.count < n {
        let f = Float(bitPattern: UInt32(truncatingIfNeeded: rng.next()))
        if f.isFinite { out.append(f) }
    }
    return out
}

func runProperty(_ r: Reporter) {
    func lst(_ a: [Float]) -> String { "[" + a.map { Double($0).description }.joined(separator: ",") + "]" }
    for shuffle in [true, false] {
        for seed in 0..<12 {
            var rng = SeededRNG(UInt64(seed) &* 2 &+ (shuffle ? 1 : 0))
            let a0 = randomFinite(&rng, Int(rng.next() % 300))
            let a1 = randomFinite(&rng, Int(rng.next() % 300))
            let src = s2b("{\"aaa\":{\"weights\":\(lst(a0))},\"weights\":\(lst(a1))}")
            let packed = Namz.pack(src, options: PackOptions(shuffle: shuffle))
            r.check(!packed.isEmpty && Namz.isNamz(packed), "pack failed seed \(seed)")
            let back = Namz.unpack(packed)
            r.check(!back.isEmpty, "unpack failed seed \(seed)")
            var got: [[Float]] = []
            if let a = try? JSONSerialization.jsonObject(with: Data(back)) { gatherFloats(a, &got) }
            r.eq(got.map(floatBits), [a0, a1].map(floatBits), "weights seed \(seed) shuffle \(shuffle)")
            r.eq(Namz.pack(back, options: PackOptions(shuffle: shuffle)), packed, "idempotence seed \(seed)")
        }
    }
    // specials (incl. -0.0, subnormals, FLT_MAX). We verify via idempotence — pack(unpack(x)) == x proves
    // every weight byte round-tripped exactly — rather than re-parsing with JSONSerialization, which drops
    // the sign of -0.0 (a reader limitation, not a codec one; see the edge-floats vector for -0.0).
    let specials: [Float] = [0, -0.0, Float(bitPattern: 1), Float.leastNormalMagnitude,
                             Float.greatestFiniteMagnitude, -Float.greatestFiniteMagnitude, 1.0 / 3.0]
    let src = s2b("{\"architecture\":\"X\",\"weights\":\(lst(specials))}")
    for shuffle in [true, false] {
        let opts = PackOptions(shuffle: shuffle)
        let packed = Namz.pack(src, options: opts)
        let back = Namz.unpack(packed)
        r.check(!back.isEmpty, "specials decode shuffle \(shuffle)")
        r.eq(Namz.pack(back, options: opts), packed, "specials bit-exact (idempotent) shuffle \(shuffle)")
        r.check(b2s(back).contains("-0.0"), "specials preserve -0.0 sign shuffle \(shuffle)")
    }
}
