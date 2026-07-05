// swift-tools-version:5.9
// SPDX-License-Identifier: MIT
import PackageDescription

let package = Package(
    name: "namz",
    products: [
        .library(name: "Namz", targets: ["Namz"]),
        .executable(name: "namz", targets: ["namz-cli"]),
    ],
    targets: [
        .target(name: "Namz"),
        .executableTarget(name: "namz-cli", dependencies: ["Namz"]),
        // Portable conformance/property/adversarial/differential runner (no XCTest dependency, so it runs
        // on a CommandLineTools-only macOS too). Invoke with `swift run namz-conformance`.
        .executableTarget(name: "namz-conformance", dependencies: ["Namz"]),
    ]
)
