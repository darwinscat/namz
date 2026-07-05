// SPDX-License-Identifier: MIT
// namz — command-line packer/unpacker, mirroring the C++ reference CLI (cli/namz.cpp).
import Foundation
import Namz

func eprint(_ s: String) { FileHandle.standardError.write(Data((s).utf8)) }

let usageText = """
namz — lossless .nam <-> .namz codec

Usage:
  namz encode <in.nam> <out.namz> [--no-shuffle] [--set key=value ...]
  namz decode <in.namz> <out.nam>
  namz map    <in.namz> [--json]        print the metadata header (no weight decode)
  namz verify <in.nam>                  pack->unpack round-trip check + ratio

"""

func usage() -> Int32 {
    eprint(usageText)
    return 2
}

func readFile(_ path: String) -> [UInt8]? {
    guard let d = try? Data(contentsOf: URL(fileURLWithPath: path)) else { return nil }
    return [UInt8](d)
}
func writeFile(_ path: String, _ bytes: [UInt8]) -> Bool {
    return (try? Data(bytes).write(to: URL(fileURLWithPath: path))) != nil
}
func pct(_ numer: Int, _ denom: Int) -> Double { denom == 0 ? 0 : 100.0 * Double(numer) / Double(denom) }

func doEncode(_ argv: [String]) -> Int32 {
    guard argv.count >= 4 else { return usage() }
    let inPath = argv[2], outPath = argv[3]
    var options = PackOptions()
    var i = 4
    while i < argv.count {
        let a = argv[i]
        if a == "--no-shuffle" {
            options.shuffle = false
        } else if a == "--set", i + 1 < argv.count {
            i += 1
            let kv = argv[i]
            guard let eq = kv.firstIndex(of: "=") else {
                eprint("bad --set (need key=value): \(kv)\n"); return 2
            }
            options.metadata[String(kv[kv.startIndex..<eq])] = String(kv[kv.index(after: eq)...])
        } else {
            eprint("unknown option: \(a)\n"); return usage()
        }
        i += 1
    }
    guard let src = readFile(inPath) else { eprint("cannot read \(inPath)\n"); return 1 }
    let packed = Namz.pack(src, options: options)
    if packed.isEmpty { eprint("encode failed (not valid NAM JSON?): \(inPath)\n"); return 1 }
    if !writeFile(outPath, packed) { eprint("cannot write \(outPath)\n"); return 1 }
    eprint(String(format: "%@ -> %@  (%d -> %d bytes, %.1f%%)\n", inPath, outPath, src.count,
                  packed.count, pct(packed.count, src.count)))
    return 0
}

func doDecode(_ argv: [String]) -> Int32 {
    guard argv.count >= 4 else { return usage() }
    let inPath = argv[2], outPath = argv[3]
    guard let src = readFile(inPath) else { eprint("cannot read \(inPath)\n"); return 1 }
    let nam = Namz.unpack(src)
    if nam.isEmpty { eprint("decode failed (not a .namz / corrupt / over cap): \(inPath)\n"); return 1 }
    if !writeFile(outPath, nam) { eprint("cannot write \(outPath)\n"); return 1 }
    eprint("\(inPath) -> \(outPath)  (\(src.count) -> \(nam.count) bytes)\n")
    return 0
}

func doMap(_ argv: [String]) -> Int32 {
    guard argv.count >= 3 else { return usage() }
    let inPath = argv[2]
    let asJson = argv.count >= 4 && argv[3] == "--json"
    guard let src = readFile(inPath) else { eprint("cannot read \(inPath)\n"); return 1 }
    if !Namz.isNamz(src) { eprint("not a .namz: \(inPath)\n"); return 1 }
    let m = Namz.readMeta(src)
    let keys = m.keys.sorted()
    if asJson {
        func esc(_ s: String) -> String { s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") }
        print("{" + keys.map { "\"\(esc($0))\":\"\(esc(m[$0]!))\"" }.joined(separator: ",") + "}")
    } else {
        for k in keys { print("  \(k) = \(m[k]!)") }
        if keys.isEmpty { eprint("(no metadata header — v1 .namz or packed without --set)\n") }
    }
    return 0
}

func doVerify(_ argv: [String]) -> Int32 {
    guard argv.count >= 3 else { return usage() }
    let inPath = argv[2]
    guard let src = readFile(inPath) else { eprint("cannot read \(inPath)\n"); return 1 }
    let packed = Namz.pack(src)
    if packed.isEmpty { eprint("FAIL pack: \(inPath)\n"); return 1 }
    let back = Namz.unpack(packed)
    if back.isEmpty { eprint("FAIL unpack: \(inPath)\n"); return 1 }
    let packed2 = Namz.pack(back)
    let idempotent = packed == packed2
    eprint(String(format: "%@: %@  raw=%d namz=%d (%.1f%%)  idempotent=%@\n", inPath,
                  idempotent ? "OK" : "MISMATCH", src.count, packed.count, pct(packed.count, src.count),
                  idempotent ? "yes" : "no"))
    return idempotent ? 0 : 1
}

let argv = CommandLine.arguments
var code: Int32 = 2
if argv.count < 2 {
    code = usage()
} else {
    switch argv[1] {
    case "encode": code = doEncode(argv)
    case "decode": code = doDecode(argv)
    case "map": code = doMap(argv)
    case "verify": code = doVerify(argv)
    default: code = usage()
    }
}
exit(code)
