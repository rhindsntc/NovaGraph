// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "NovaGraph",
    platforms: [.macOS(.v13), .iOS(.v16), .tvOS(.v16), .watchOS(.v9), .macCatalyst(.v16)],
    products: [.library(name: "GraphDBKit", targets: ["GraphDBKit"])],
    targets: [
        .target(
            name: "CGraphDB",
            path: "cpp",
            exclude: ["tests", "CMakeLists.txt"],
            sources: ["src"],
            publicHeadersPath: "c_api",
            cxxSettings: [.headerSearchPath("include"), .headerSearchPath("third_party")]
        ),
        .target(name: "GraphDBKit", dependencies: ["CGraphDB"], path: "swift/Sources/GraphDBKit"),
        .testTarget(name: "GraphDBKitTests", dependencies: ["GraphDBKit"], path: "swift/Tests/GraphDBKitTests")
    ],
    cxxLanguageStandard: .cxx20
)
