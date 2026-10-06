// swift-tools-version: 6.0
import PackageDescription
let package = Package(
    name: "IntegrationSmoke", platforms: [.macOS(.v13), .iOS(.v16), .tvOS(.v16), .watchOS(.v9), .macCatalyst(.v16)],
    dependencies: [.package(name: "NovaGraph", path: "../..")],
    targets: [.executableTarget(name: "IntegrationSmoke", dependencies: [.product(name: "GraphDBKit", package: "NovaGraph")])]
)
