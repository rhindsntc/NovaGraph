<div align="center">

<img src="docs-web/public/images/novagraph-mark.svg" width="88" height="88" alt="NovaGraph graph mark">

# NovaGraph

**An embedded property graph database for Swift.**

Nodes, relationships, and queries—inside your application.

[![Swift 6.0+](https://img.shields.io/badge/Swift-6.0%2B-F05138?logo=swift&logoColor=white)](Package.swift)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-6366f1)](Package.swift)
[![Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue)](LICENSE)

[Installation](#installation) · [Quick start](#quick-start) · [Examples](#examples) · [Documentation](#documentation) · [Roadmap](TODO.md)

</div>

## Why NovaGraph?

Represent connected data as nodes and typed, directed edges. Work through `GraphDBKit`, a Swift API backed by a C++20 storage engine and C ABI. The source package builds the engine with your application; no database server or manual native linker flags are needed.

| Capability | What you can do |
| --- | --- |
| **Graph storage** | Store nodes, edges, flat scalar properties, and property indexes. |
| **Swift types** | Use `GraphValue`, flat `Codable` models, typed results, and errors. |
| **Traversal** | Follow inbound/outbound edges and retrieve one deterministic shortest-hop path per result. |
| **NGQL** | Query with parameters, projections, limits, and explain output. |
| **Transactions** | Commit mutation batches atomically with WAL-backed storage and checkpoints. |
| **Application lifecycle** | Use cancellation, snapshots, explicit close, and backup/restore tools. |

## Installation

**Requirements:** Swift 6.0+ and an Apple development toolchain. The core package includes its native sources and has no remote package dependencies. CMake is needed for native tests/tools, not SwiftPM installation.

### Get the source

Unpack a source archive into a directory named `Nova`, or clone the repository. The Git command below is a template: replace `REPOSITORY_URL` with the actual repository URL; a public URL/tag has not been selected yet.

```sh
git clone REPOSITORY_URL Nova
cd Nova
swift build
swift test
```

### Swift Package Manager

Place your application package beside the `Nova` checkout. This complete example manifest builds an executable named `Example`; place the quick-start code below in `Sources/Example/main.swift` and run `swift run Example` from that application package.

```swift
// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "Example",
    platforms: [.macOS(.v13)],
    dependencies: [
        .package(name: "NovaGraph", path: "../Nova")
    ],
    targets: [
        .executableTarget(
            name: "Example",
            dependencies: [
                .product(name: "GraphDBKit", package: "NovaGraph")
            ]
        )
    ]
)
```

For a hosted Git dependency, pin the actual repository URL to a reviewed commit or published version and match the product's package identity to that URL. See the [installation guide](docs-web/content/quickstart.md#add-graphdbkit-to-an-application) for those templates.

### Xcode

1. Add the **repository root** as a local Swift package.
2. Select the **GraphDBKit** library product for your app target.
3. Add `import GraphDBKit` to your Swift source.

Swift consumers use the C boundary; enabling C++ interoperability or adding manual `-L`/`-l` flags is unnecessary.

## Quick start

Create a small graph, commit it as one transaction, and follow a relationship:

```text
ada ──FOLLOWS──▶ grace
```

```swift
import Foundation
import GraphDBKit

// A disposable directory for this example.
let directory = FileManager.default.temporaryDirectory
    .appendingPathComponent("nova-\(UUID().uuidString)")
let db = try GraphDatabase(path: directory)

try db.createIndex(label: "Person", property: "email")
try db.transaction { tx in
    tx.upsertNode(label: "Person", id: "ada", properties: [
        "name": "Ada", "email": "ada@example.com"
    ])
    tx.upsertNode(label: "Person", id: "grace", properties: [
        "name": "Grace", "email": "grace@example.com"
    ])
    tx.upsertEdge(type: "FOLLOWS", from: "ada", to: "grace")
}

let friends = try db.from("ada").out("FOLLOWS").depth(1).collect()
print(friends.map(\.id)) // ["grace"]

try db.close()
try FileManager.default.removeItem(at: directory)
```

Validation failures leave live data unchanged. WAL write or sync failures can have an unknown commit outcome; read the [error contracts](docs-web/content/errors.md) before retrying. For an application, choose an appropriate storage directory and manage opening/closing through its lifecycle; see the [runnable Apple sample](examples/NovaGraphApp/README.md).

## Examples

The following Swift snippets assume an **open** `db` containing the quick-start graph. Run them before `db.close()` and cleanup.

### Codable properties

```swift
struct Person: Codable, Sendable {
    let name: String
    let email: String
}

try db.upsertNode(label: "Person", id: "ada", model:
    Person(name: "Ada", email: "ada@example.com"))
let person = try db.node(id: "ada", as: Person.self)
print(person?.properties.name ?? "Not found") // Ada
```

Properties are flat scalars: strings, Int64 integers, finite doubles, booleans, and null. Nested Codable payloads are rejected. An upsert replaces the record's properties; see [values and identity](docs-web/content/values.md).

### Indexed lookup

```swift
let matches = try db.nodes(
    label: "Person",
    where: "email",
    equals: .string("ada@example.com")
)
print(matches.map(\.id)) // ["ada"]
```

### Real traversal paths

```swift
let paths = try db.from("ada").out("FOLLOWS").depth(2).paths()
print(paths.first?.nodes.map(\.id) ?? []) // ["ada", "grace"]
```

Paths include the actual nodes and edges. Traversal uses breadth-first search and selects one shortest-hop route per reachable node; it does not enumerate every possible path. See [traversal semantics](docs-web/content/ngql-syntax.md#deterministic-traversal-and-paths).

### NGQL

The same graph can be created and queried with NGQL:

```ngql
create index on Person(email);
upsert node Person ada set name="Ada", email="ada@example.com";
upsert node Person grace set name="Grace";
upsert edge FOLLOWS ada -> grace set since=2026;
get node ada;
find nodes Person where email = "ada@example.com" limit 10;
walk from ada over FOLLOWS depth 2 limit 25;
```

Bind values instead of interpolating query text:

```swift
let people = try db.queryResult(
    "find nodes Person where email = $email limit 10",
    parameters: ["email": .string("ada@example.com")]
).nodes()
print(people.map(\.id)) // ["ada"]
```

Use `mutate` for NGQL writes and `queryResult` for typed reads. See the [NGQL reference](docs-web/content/ngql-syntax.md) and [Swift API](docs-web/content/graph-database.md) for projections, mutation receipts, snapshots, and asynchronous operations.

## Apple platforms

| Platform | Declared deployment target | Qualification |
| --- | --- | --- |
| macOS | 13+ | Development checks; oldest runtime unqualified |
| iOS | 16+ | Simulator checks and manual iPhone 17 Pro Max / iOS 27.0 smoke pass; full device and oldest-runtime qualification open |
| tvOS | 16+ | Runtime qualification incomplete |
| watchOS | 9+ | Runtime qualification incomplete |
| Mac Catalyst | 16+ | Runtime qualification incomplete |

Declared targets are not a verified runtime-support matrix. Binary deployment floors can vary by architecture/SDK; visionOS is unsupported. NovaGraph permits one process to own a database directory at a time. Accounted resource limits are not a strict total-process-memory ceiling. Read [compatibility and limitations](docs-web/content/compatibility.md) before evaluating an existing database or choosing a platform.

## Documentation

| Start here | Learn more |
| --- | --- |
| [Installation and quickstart](docs-web/content/quickstart.md) | [Swift API](docs-web/content/graph-database.md) · [Traversal builder](docs-web/content/traversal-builder.md) |
| [NGQL language](docs-web/content/ngql-syntax.md) | [Parameters, projections, explain](docs-web/content/ngql-advanced.md) |
| [Architecture](docs-web/content/architecture.md) | [Storage](docs-web/content/storage-engine.md) · [Transactions](docs-web/content/transactions.md) |
| [Errors and recovery](docs-web/content/errors.md) | [Recovery](docs-web/content/recovery.md) · [Backup and restore](docs-web/content/backup-restore.md) |
| [Apple lifecycle](docs-web/content/swiftui-lifecycle.md) | [Sample application](examples/NovaGraphApp/README.md) |
| [Testing](docs-web/content/testing.md) | [Benchmarks and measurement limits](docs-web/content/benchmarks-data.md) · [Public TODOs](TODO.md) |

### Run the developer website

With Node.js 22 and npm installed:

```sh
npm --prefix docs-web ci
make docs-dev
```

The website is generated from public guides and executable examples. Its static playground shows recorded engine results; opt-in live queries use the [local developer runner](docs-web/content/developer-tools.md), which requires Swift 6.1+.

### Host on GitHub Pages

A Pages workflow builds and deploys `docs-web/dist` from `main`, using the repository's configured site path automatically. After creating the public repository, enable **Settings → Pages → GitHub Actions**, then run **Deploy documentation to Pages**. See the [hosting guide](docs-web/content/documentation.md#github-pages-hosting) for setup and local preview commands.

### Try the demos and checks

```sh
swift run --package-path examples/NovaGraphDemo NovaGraphDemo
make typed-api-example
make docs-check
```

Native development checks additionally require CMake 3.22+ and a C++20 compiler: `make cpp-test`. See [contributing](CONTRIBUTING.md) for the full workflow. Passing these checks does not certify a production release.

## Roadmap and contributing

The [public TODO roadmap](TODO.md) tracks publication setup and unfinished quality/soak, Apple platform, and production-release qualification. Fuzz work remains deferred. Small, scoped contributions and synthetic reproductions are welcome; please read [CONTRIBUTING.md](CONTRIBUTING.md) and use [SECURITY.md](SECURITY.md) for private vulnerability reporting.

## License

NovaGraph is licensed under [Apache-2.0](LICENSE). Preserve the [third-party notices](licenses/README.md) when redistributing the source or artifacts. The [release checklist](RELEASING.md) separates source publication from production qualification.
