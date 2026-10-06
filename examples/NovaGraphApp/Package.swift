// swift-tools-version: 6.0
import PackageDescription
let package = Package(
    name: "NovaSample", platforms: [.macOS(.v13), .iOS(.v16)],
    products: [.library(name: "NovaSample", targets: ["NovaSample"])],
    dependencies: [.package(name: "NovaGraph", path: "../..")],
    targets: [
        .target(name: "NovaSample", dependencies: [.product(name: "GraphDBKit", package: "NovaGraph")]),
        .testTarget(name: "NovaSampleTests", dependencies: ["NovaSample", .product(name: "GraphDBKit", package: "NovaGraph")])
    ]
)
