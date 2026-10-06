# Local Quickstart

Nova's root `Package.swift` builds the C++20 engine and the `GraphDBKit` Swift module together. SwiftPM supplies the native linkage. The core package has no remote dependencies; its pinned JSON header and license ship in the repository.

## Build from a checkout

Use Swift 6.0 or newer from the repository root:

```sh
swift build
swift test
swift run --package-path examples/NovaGraphDemo NovaGraphDemo
```

CMake is needed only for native tests and native maintenance tools, not for Swift installation. Those workflows require CMake 3.22 or newer on PATH, or explicit `CMAKE`/`CTEST` settings. The local developer runner additionally needs Swift 6.1 or newer for its pinned SwiftNIO dependency.

## Add GraphDBKit to an application

In Xcode, add the **repository root** as a local package or use its Git repository URL at a commit/tag containing this root package. Select the **GraphDBKit** library product and `import GraphDBKit`. Swift consumers do not enable C++ interoperability: only the C ABI is exported to the Swift wrapper.

For a local SwiftPM consumer, use `.package(name: "NovaGraph", path: "/absolute/path/to/Nova")` and `.product(name: "GraphDBKit", package: "NovaGraph")`. For a Git dependency, use `.package(url: repositoryURL, revision: commitSHA)` or a published version requirement, and match the product's `package` argument to the repository URL's package identity. These are manifest templates; replace the path/URL/revision with your checkout or actual hosted repository.

No hosted package URL or release is promised here. Use the actual repository location and revision you have chosen. Local source-package checks do not certify a hosted URL, a physical Apple device or an older runtime. See [compatibility](compatibility.md#source-package-and-runtime-qualification).

## Migrate from the nested package

The root manifest is authoritative. Replace dependencies on `Nova/swift` with `Nova`, remove application `-L`/`-l` and `-Xlinker` flags for `novagraphdb`, and use ordinary SwiftPM commands. The old `swift/Package.swift` and duplicate C-header copy are removed; Swift source files remain under `swift/Sources` as an internal layout. Do not add those files as separate application targets. `make swift-build`, `make swift-test`, the examples, runner and benchmarks all consume the root package.

## Nodes and edges

<!-- snippet: swift-basics -->

## Codable helpers

<!-- snippet: swift-codable -->

Use flat scalar properties. Nested payloads are rejected before mutation. Swift preserves integer 0/1, Int64 bounds, finite Double values and Bool distinctly; see [values](values.md). Legacy formats require the explicit importer into a fresh destination. Run `make typed-api-example` for the compiled typed bindings/projection/path example.

## Atomic batches

<!-- snippet: swift-batch -->

The joined mutating program is atomic. All statements must parse, bind and validate before durable commit; maintenance commands are rejected inside it. See [transactions](transactions.md).

## Traversal

<!-- snippet: swift-traversal -->

See [testing](testing.md) for the development and strict release checks, and [API semantics](graph-database.md) before relying on typed results.

## Run the Apple sample

The [runnable macOS/iOS sample](../../examples/NovaGraphApp/README.md) has Graph, Query and Lifecycle screens backed by the public package. Run `make sample-app-generate`, open `examples/NovaGraphApp/NovaGraphApp.xcodeproj`, choose the macOS or iOS scheme and a Mac, iPhone simulator or connected iPhone, then Run. Physical iPhones require your own development signing team. XcodeGen 2.45 or newer is required; no manual native linker flags are needed.

Its real database-opening helper keeps synchronous recovery off the UI actor:

<!-- snippet: sample-opening -->
