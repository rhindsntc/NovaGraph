# Fluent Traversal

The Swift traversal API is an immutable Sendable fluent builder over native traversal operations. It is not a Swift result-builder DSL.

<!-- snippet: swift-traversal -->

The implementation uses breadth-first traversal with visited-node tracking. Each direction/depth/limit method returns a new builder value; copies retain their independent settings. Repeating directions replaces the direction in that returned value rather than constructing a general path expression. Native/NGQL path materialization is implemented through the explicit `paths` clause. Swift `collect()` returns nodes, `collect(as:)` returns typed nodes, and `paths()` returns ordered `Path` values with actual edges. `db.paths(...)` returns both reachable nodes and paths. See [path semantics and a runnable example](ngql-syntax.md#deterministic-traversal-and-paths).

Generated traversal requests use the native query depth, result, expanded-edge and allocation limits. Async collection propagates Swift cancellation through the native token. Builder copies are safe to configure independently. To hold one view across multiple reads, use `readSnapshot` and its synchronous query methods; see [lifecycle](swiftui-lifecycle.md).

See [API semantics](graph-database.md) and [limitations](compatibility.md).
