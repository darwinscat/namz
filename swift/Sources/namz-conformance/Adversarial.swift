// SPDX-License-Identifier: MIT
import Foundation
import Namz

private func u32(_ v: UInt32) -> [UInt8] {
    [UInt8(v & 0xff), UInt8((v >> 8) & 0xff), UInt8((v >> 16) & 0xff), UInt8((v >> 24) & 0xff)]
}
private func synth(_ skeleton: String, _ numArrays: UInt32, _ lengths: [UInt32], _ payload: [UInt8],
                   flags: UInt8 = 0) -> [UInt8] {
    let skel = Array(skeleton.utf8)
    var body = u32(UInt32(skel.count)) + skel + u32(numArrays)
    for ln in lengths { body += u32(ln) }
    body += payload
    return [0x4e, 0x41, 0x4d, 0x5a, 2, 0, 0, flags, 0, 0] + body
}
private func f32le(_ x: Float) -> [UInt8] {
    let b = x.bitPattern
    return [UInt8(b & 0xff), UInt8((b >> 8) & 0xff), UInt8((b >> 16) & 0xff), UInt8((b >> 24) & 0xff)]
}

// MARK: Adversarial — actively try to break the codec

func runAdversarial(_ r: Reporter) {
    guard let dir = conformanceDir() else { print("  (no vectors — adversarial skipped)"); return }
    let full = readBytes(dir + "/vectors/flat.namz")
    r.check(!Namz.unpack(full).isEmpty, "flat.namz must decode")

    // unpack is total on garbage (never crashes)
    var rng = SeededRNG(1234)
    for it in 0..<3000 {
        let n = Int(rng.next() % 64)
        var blob = (0..<n).map { _ in UInt8(truncatingIfNeeded: rng.next()) }
        if it % 2 == 0 && n >= 4 { blob[0] = 0x4e; blob[1] = 0x41; blob[2] = 0x4d; blob[3] = 0x5a }
        _ = Namz.unpack(blob)
    }
    r.check(true, "unpack survived 3000 garbage inputs")

    // every truncation and extension rejected
    var truncOK = true
    for L in 0..<full.count where !Namz.unpack(Array(full[0..<L])).isEmpty { truncOK = false }
    r.check(truncOK, "some truncation was accepted")
    for k in [1, 2, 3, 4, 5, 7, 16, 64, 4096] {
        r.check(Namz.unpack(full + [UInt8](repeating: 0, count: k)).isEmpty, "+\(k) extension accepted")
    }

    // header-critical byte corruption rejected
    for pos in 0...6 {
        var b = full; b[pos] ^= 0xff
        r.check(Namz.unpack(b).isEmpty, "header byte \(pos) corruption accepted")
    }
    func withByte(_ pos: Int, _ v: UInt8) -> [UInt8] { var b = full; b[pos] = v; return b }
    r.check(Namz.unpack(withByte(4, 9)).isEmpty, "version 9 accepted")
    r.check(Namz.unpack(withByte(5, 1)).isEmpty, "codec 1 accepted")
    r.check(Namz.unpack(withByte(6, 1)).isEmpty, "dtype 1 accepted")

    // any accepted single-byte mutation is a fixed point
    var fixedPointOK = true
    for pos in 0..<full.count {
        for mask in [UInt8(0x01), 0x80, 0xff] {
            var b = full; b[pos] ^= mask
            let out = Namz.unpack(b)
            if !out.isEmpty && Namz.unpack(Namz.pack(out)) != out { fixedPointOK = false }
        }
    }
    r.check(fixedPointOK, "an accepted mutation was not a fixed point")

    // lying framing rejected
    r.check(Namz.unpack(synth("{\"weights\":0}", 0xffffffff, [], [])).isEmpty, "huge numArrays accepted")
    r.check(Namz.unpack(synth("{\"weights\":0}", 1, [1000], f32le(1.5))).isEmpty, "lying length accepted")
    r.check(Namz.unpack(synth("{\"weights\":0}", 1, [1], [0, 0, 0])).isEmpty, "non-mult-of-4 accepted")
    r.check(Namz.unpack(synth("{\"weights\":5}", 0, [], [])).isEmpty, "out-of-range index accepted")

    // corrupt skeleton JSON rejected
    let skel = Array("{not json".utf8)
    r.check(Namz.unpack([0x4e, 0x41, 0x4d, 0x5a, 2, 0, 0, 0, 0, 0] + u32(UInt32(skel.count)) + skel + u32(0)).isEmpty,
            "corrupt skeleton accepted")

    // non-finite in/out rejected
    for s in ["{\"architecture\":\"X\",\"weights\":[1e400]}", "{\"architecture\":\"X\",\"weights\":[3.5e38]}",
              "{\"architecture\":\"X\",\"gain\":1e400,\"weights\":[0.5]}", "not json", ""] {
        r.check(Namz.pack(Array(s.utf8)).isEmpty, "pack should reject: \(s.prefix(30))")
    }
    for bad: [UInt8] in [[0, 0, 0xc0, 0x7f], [0, 0, 0x80, 0x7f], [0, 0, 0x80, 0xff]] {
        r.check(Namz.unpack(synth("{\"weights\":0}", 1, [1], bad)).isEmpty, "non-finite payload accepted")
    }

    // max-json cap enforced
    let big = s2b("{\"architecture\":\"X\",\"weights\":[" + (0..<5000).map { String($0) }.joined(separator: ",") + "]}")
    let packed = Namz.pack(big)
    r.check(!packed.isEmpty, "big pack failed")
    r.check(Namz.unpack(packed, maxJSONBytes: 64).isEmpty, "over-cap not refused")
    r.check(!Namz.unpack(packed, maxJSONBytes: Namz.defaultMaxJSONBytes).isEmpty, "under-cap refused")
}
