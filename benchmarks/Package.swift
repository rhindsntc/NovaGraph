// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "NovaBenchmarks",
    platforms: [.macOS(.v13)],
    dependencies: [
        .package(name: "NovaGraph", path: ".."),
    ],
    targets: [
        .executableTarget(name: "NovaSwiftOverhead", dependencies: [.product(name: "GraphDBKit", package: "NovaGraph")]),
        .executableTarget(
            name: "NovaBenchmark",
            dependencies: [
                .product(name: "GraphDBKit", package: "NovaGraph"),
            ]
        )
    ]
)
