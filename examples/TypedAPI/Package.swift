// swift-tools-version: 6.0
import PackageDescription
let package = Package(
    name: "TypedAPI", platforms: [.macOS(.v13)],
    dependencies: [.package(name: "NovaGraph", path: "../..")],
    targets: [.executableTarget(name: "TypedAPI", dependencies: [.product(name: "GraphDBKit", package: "NovaGraph")])]
)
