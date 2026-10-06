// swift-tools-version: 6.1
import PackageDescription

let package = Package(
    name: "NovaGraphDevTools",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "NovaDevQuery", targets: ["NovaDevQuery"]),
    ],
    dependencies: [
        .package(name: "NovaGraph", path: ".."),
        .package(url: "https://github.com/apple/swift-nio.git", exact: "2.103.0"),
    ],
    targets: [
        .executableTarget(
            name: "NovaDevQuery",
            dependencies: [
                .product(name: "GraphDBKit", package: "NovaGraph"),
                .product(name: "NIO", package: "swift-nio"),
                .product(name: "NIOHTTP1", package: "swift-nio"),
                .product(name: "NIOPosix", package: "swift-nio"),
            ]
        ),
        .testTarget(name: "NovaDevQueryTests", dependencies: [
            "NovaDevQuery", .product(name: "GraphDBKit", package: "NovaGraph"),
            .product(name: "NIO", package: "swift-nio"),
            .product(name: "NIOHTTP1", package: "swift-nio"),
            .product(name: "NIOPosix", package: "swift-nio"),
        ])
    ]
)
