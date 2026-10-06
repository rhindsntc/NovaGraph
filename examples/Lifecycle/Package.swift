// swift-tools-version: 6.0
import PackageDescription
let package = Package(
    name: "Lifecycle", platforms: [.macOS(.v13), .iOS(.v16)],
    dependencies: [.package(name: "NovaGraph", path: "../..")],
    targets: [.executableTarget(name: "Lifecycle", dependencies: [.product(name: "GraphDBKit", package: "NovaGraph")])]
)
