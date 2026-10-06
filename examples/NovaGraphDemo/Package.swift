// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "NovaGraphDemo",
    platforms: [
        .macOS(.v13),
        .iOS(.v16)
    ],
    dependencies: [
        .package(name: "NovaGraph", path: "../..")
    ],
    targets: [
        .executableTarget(
            name: "NovaGraphDemo",
            dependencies: [
                .product(name: "GraphDBKit", package: "NovaGraph")
            ]
        )
    ]
)
