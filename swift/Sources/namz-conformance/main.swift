// SPDX-License-Identifier: MIT
// Portable conformance/property/adversarial/unit/differential runner for the Swift port.
// Run with `swift run namz-conformance` (no XCTest dependency; works anywhere Swift builds).
import Foundation

let r = Reporter()

print("conformance (shared TCK vectors)…")
runConformance(r)
print("unit (public API)…")
runUnit(r)
print("property (random finite float32 round-trip)…")
runProperty(r)
print("adversarial (falsification)…")
runAdversarial(r)
print("differential (vs C++ reference CLI)…")
runDifferential(r)

print("\n\(r.failures == 0 ? "PASS" : "FAIL"): \(r.checks) checks, \(r.failures) failures")
exit(r.failures == 0 ? 0 : 1)
