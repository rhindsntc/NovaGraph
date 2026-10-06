---
name: novagraph
description: Use when integrating NovaGraph (GraphDBKit) into a Swift or SwiftUI app, writing NGQL queries, modeling nodes and edges, or handling NovaGraph transactions, traversal, persistence, and lifecycle errors.
---

# NovaGraph

NovaGraph is an embedded property graph database for Apple platforms. Its `GraphDBKit` Swift library wraps a C++20 engine through a C ABI; it needs no database server.

## Find the right reference

Use the application's resolved package revision as the source of truth. In a checkout, read the matching files under `docs-web/content/` and `swift/Sources/GraphDBKit/`. Otherwise use the [public repository](https://github.com/rhindsntc/NovaGraph); select the matching revision before relying on an API. Hosted `main` may differ from an installed version.

| Task | Reference in the NovaGraph checkout |
| --- | --- |
| Install, models, typed reads/writes | `Package.swift`, `docs-web/content/quickstart.md`, `graph-database.md`, `values.md` |
| NGQL grammar, bindings, limits, paths | `docs-web/content/ngql-syntax.md`, `ngql-advanced.md`, `traversal-builder.md` |
| SwiftUI ownership and background work | `docs-web/content/swiftui-lifecycle.md`, `examples/NovaGraphApp/Sources/NovaSample/` |
| Errors, durability, migration, backup | `docs-web/content/errors.md`, `transactions.md`, `compatibility.md`, `backup-restore.md` |

Bare Markdown filenames in this table also live under `docs-web/content/`. Check compatibility and the public roadmap before making support or production-readiness claims.

## Integration

- Add the **root** package from `https://github.com/rhindsntc/NovaGraph.git`, choosing an existing reviewed revision or published version. Do not invent a version tag. Select product `GraphDBKit` and `import GraphDBKit`.
- Core requirements: Swift 6.0+ and an Apple toolchain. SwiftPM builds and links the native sources; C++ interoperability and manual linker flags are unnecessary. The old nested `swift/` package is not the entry point.
- Open with `try GraphDatabase(path: databaseURL)`, where the URL identifies a local database directory. Use an app-owned persistent directory for real data; temporary directories are for examples/tests.
- Keep one database owner per directory. A second open returns `busy`; an app and extension cannot concurrently own that directory. Suspend retains ownership.

## Small working example

This synchronous command-line example uses disposable storage. In SwiftUI, keep synchronous opening/recovery off `MainActor` as the sample's `DatabaseOpener` actor does, then await asynchronous operations.

```swift
import Foundation
import GraphDBKit

let directory = FileManager.default.temporaryDirectory
    .appendingPathComponent("novagraph-\(UUID().uuidString)")
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

let matches = try db.queryResult(
    "find nodes Person where email = $email limit 10",
    parameters: ["email": .string("ada@example.com")]
).nodes()
let paths = try db.from("ada").out("FOLLOWS").depth(2).limit(10).paths()
print(matches.map(\.id))                 // ["ada"]
print(paths.map { $0.nodes.map(\.id) })  // [["ada", "grace"]]

try db.close()
try FileManager.default.removeItem(at: directory)
```

## Semantics that change implementation choices

- Properties are flat `GraphValue` scalars: String, Int64, finite Double, Bool, null. Codable models use `Codable & Sendable`; nested objects and arrays are rejected. Represent related entities as nodes/edges or deliberately flatten fields.
- Upserts **replace** all properties, rather than patching omitted fields. Node identity is its ID; edge identity is `(from, type, to)`. Supply the full intended property set. Both endpoints must exist in the transaction's staged state.
- Prefer `node(id:)`, `nodes(...)`, `queryResult(...).nodes()` and other typed APIs. Only `notFound` becomes nil for point reads; storage and decode errors throw. Use `mutate` or `transactionReceipt` when a typed commit receipt is needed.
- Bind `$name` through `[String: GraphValue]`, without the `$` in dictionary keys. NGQL is not SQL or Cypher: use its documented `find`, `walk`, `get`, `upsert` grammar. Maintenance commands cannot share a program with logical mutations.
- Traversal is breadth-first, up to the requested depth, with one deterministic shortest-hop path per reachable node. It excludes the start; it does not enumerate every path. Each `Path` exposes ordered `nodes` and `edges`; an edge's `from`/`to` retain stored direction even in inbound traversal. Builder methods return new values; `.out().in()` does not compose multiple traversal steps. NGQL path mode is the bare `paths` clause, not `return paths`.
- Precommit validation failures publish no writes. `commitOutcomeUnknown` can mean the mutation survives recovery: preserve its transaction context and reconcile after reopening; never blindly retry. Receipts are not reusable idempotency keys.
- Await asynchronous APIs from UI tasks. Keep one app-owned `GraphLifecycle`; handle errors from background/foreground/memory-pressure tasks and explicitly close. Snapshot callbacks are synchronous, read-only and short: no awaits or reentrant database calls.
- Query options only tighten supported ceilings. Cancellation/deadlines are cooperative, and memory accounting is not a hard process-RSS cap. Use `checkpoint()` for Swift maintenance; trim evicts eligible payloads without deleting logical records.

## Verify the requested work

Compile against the resolved package and check the behavior changed: for persistence, close/reopen; for traversal, assert actual node and edge paths. Keep examples synthetic. The hosted playground displays recorded results, not live NGQL execution.

When modifying NovaGraph itself, follow `CONTRIBUTING.md`. Update canonical guides and executable examples with API changes, regenerate documentation rather than editing generated JSON, and run the relevant native/Swift checks plus `make docs-check` for documentation changes.
