// SPDX-License-Identifier: MIT
import Foundation

/// Tiny test reporter — tracks checks/failures without a test framework (portable to any environment).
final class Reporter {
    var checks = 0
    var failures = 0
    func check(_ cond: Bool, _ msg: @autoclosure () -> String) {
        checks += 1
        if !cond {
            failures += 1
            print("  FAIL: \(msg())")
        }
    }
    func eq<T: Equatable>(_ a: T, _ b: T, _ msg: @autoclosure () -> String) { check(a == b, msg()) }
}

/// Deterministic PRNG (SplitMix64-style) so fuzz runs are reproducible.
struct SeededRNG: RandomNumberGenerator {
    var state: UInt64
    init(_ seed: UInt64) { state = seed &+ 0x9e3779b97f4a7c15 }
    mutating func next() -> UInt64 {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        var z = state
        z = (z ^ (z >> 30)) &* 0xbf58476d1ce4e5b9
        z = (z ^ (z >> 27)) &* 0x94d049bb133111eb
        return z ^ (z >> 31)
    }
}

/// Walk up from this source file until we find conformance/manifest.json (monorepo: ../../conformance).
func conformanceDir(file: String = #filePath) -> String? {
    var dir = URL(fileURLWithPath: file).deletingLastPathComponent()
    for _ in 0..<8 {
        let cand = dir.appendingPathComponent("conformance/manifest.json")
        if FileManager.default.fileExists(atPath: cand.path) {
            return dir.appendingPathComponent("conformance").path
        }
        dir = dir.deletingLastPathComponent()
    }
    return nil
}

func readBytes(_ path: String) -> [UInt8] {
    [UInt8]((try? Data(contentsOf: URL(fileURLWithPath: path))) ?? Data())
}

/// Collect numeric "weights" arrays (bools excluded) in sorted-key DFS order as [Float]. Uses the default
/// String sort — order need only be *consistent* between the two trees being compared, not nlohmann's.
func gatherFloats(_ obj: Any, _ out: inout [[Float]]) {
    if let dict = obj as? [String: Any] {
        for key in dict.keys.sorted() {
            let val = dict[key]!
            if key == "weights", let arr = val as? [Any],
               arr.allSatisfy({ !($0 is Bool) && ($0 is NSNumber) }) {
                out.append(arr.map { Float(($0 as! NSNumber).doubleValue) })
            } else {
                gatherFloats(val, &out)
            }
        }
    } else if let arr = obj as? [Any] {
        for item in arr { gatherFloats(item, &out) }
    }
}

func floatBits(_ a: [Float]) -> [UInt32] { a.map { $0.bitPattern } }
func b2s(_ b: [UInt8]) -> String { String(decoding: b, as: UTF8.self) }
func s2b(_ s: String) -> [UInt8] { Array(s.utf8) }
