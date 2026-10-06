# GraphDatabase API

GraphDBKit provides typed scalar bindings, node/edge/path results, Codable projections and durable mutation receipts. Native failures retain stable codes and context. Cancellation, snapshots and explicit close are available. Nova remains experimental. Platform and release qualification remain open.

## Configuration

`GraphDatabase(configuration:)` takes `GraphDatabaseConfiguration(path:inactivityInterval:sweepInterval:queryOptions:)`. The convenience `GraphDatabase(path:inactivityInterval:sweepInterval:)` defaults to a 600-second TTL and 30-second sweep interval. Paths must be local file URLs without NUL. A second database owner throws `GraphCoreError` with code `busy`.

`GraphQueryOptions` exposes `maxDepth`, `maxResults`, `maxExpandedEdges`, `workingBytes`, `resultBytes`, `maxStatements`, `batchBytes` and `timeoutMilliseconds`. Defaults come from the native engine. Database and per-query options may lower the supported ceilings. See [NGQL configuration](ngql-syntax.md#configuration).

## Typed reads

All operations below throw on failure:

| Method | Result |
|---|---|
| `node(id:)` | `Node?` |
| `node(id:as:)` | `TypedNode<Props>?` |
| `nodes(label:where:equals:limit:)` | `[Node]` |
| `nodes(label:where:equals:as:limit:)` | `[TypedNode<Props>]` |
| `edge(from:type:to:)` | `Edge?` |
| `edge(from:type:to:as:)` | `TypedEdge<Props>?` |
| `traverse(from:over:direction:depth:limit:)` | `[Node]` |
| `paths(from:over:direction:depth:limit:)` | `GraphTraversalResult` with `[Node]` and `[Path]` |

Only a native `notFound` becomes nil in point reads. IO, corrupt data, parse and decode failures throw. Collection decoding never silently substitutes an empty array. `Node` and `Edge` retain exact `[String: GraphValue]` properties and expose `decode(as:)`. Typed model constraints are `Codable & Sendable`.

## Typed queries, projections and receipts

- `queryResult(_:parameters:options:) -> GraphQueryResult` accepts `[String: GraphValue]` (default empty) and optional `GraphQueryOptions`.
- `query(_:parameters:as:options:) -> T` decodes `T: Decodable`; projections can use ordinary app structs.
- `GraphQueryResult` contains `data: GraphJSON` and optional `receipt: GraphMutationReceipt`. Its `decode(as:)` removes scalar wire tags for Codable. `node()`, `nodes()`, `edge()` and `traversal()` require full records and preserve graph scalar types.
- `mutate(_:parameters:options:) -> GraphMutationReceipt` requires a successfully committed mutation; read-only programs produce no receipt.
- `transactionReceipt(_:) -> GraphMutationReceipt?` stages a `GraphTransaction`; an empty block returns nil. Existing `transaction(_:) -> Void` remains available.

A receipt has `transactionId: String` and `committedLSN: UInt64`. A failed batch has no receipt and applies no earlier statements. An ambiguous native commit throws `commitOutcomeUnknown` with transaction context; do not automatically retry it.

<!-- snippet: swift-typed-api -->

This exact source compiles and runs via `make typed-api-example`. It checks Int64.max, Codable projection, typed edges, a commit receipt and real traversal edges. The example uses a temporary directory; the broader executable docs harness runs through `make docs-examples`.

## Mutations and Codable properties

`upsertNode(label:id:properties:)`, `upsertNode(label:id:model:)`, `upsertEdge(type:from:to:properties:)`, `upsertEdge(type:from:to:model:)` and `createIndex(label:property:)` retain their existing JSON String return type. Use `mutate` or `transactionReceipt` when a typed receipt is needed. `deleteNode(id:)` and `deleteEdge(type:from:to:)` return Void and throw errors.

Codable models encode through a flat scalar encoder, preserving an integral Double as Double and rejecting arrays, nested objects, unsigned overflow and nonfinite numbers before submitting a mutation. Custom single-value Encodable properties are supported if they encode one supported scalar. Swift convenience labels, edge types and property names use `[A-Za-z_][A-Za-z0-9_]*`; transaction builders enforce the same restrictions before submission. IDs may contain Unicode, delimiters, quotes and NUL.

Upserts replace properties. Edge endpoints must exist in staged state. See [transaction semantics](transactions.md) and [scalar values](values.md).

## Raw API and response ownership

`rawQuery(_:parameters:) -> String` preserves the unwrapped payload JSON shape and accepts legacy `[String: Any]?` scalar bindings. Bool is distinguished from NSNumber by its Foundation type identity; nested/unsupported values throw. Prefer `[String: GraphValue]` in `queryResult` for unambiguous scalar types. `query(_:parameters:)`, `getNode`, `findNodes` and `walk` are deprecated with typed/raw replacements. `execute` remains a raw compatibility alias. Raw output preserves byte-distinct property names and native numeric text. Swift dictionaries equate canonically equivalent Unicode strings, so typed result decoding explicitly rejects duplicate or Unicode-equivalent keys; use rawQuery to read such records without dropping data. Record and projection containers are preserved even when their fields are named type and value. Raw JSON cannot carry graph scalar types reliably through every consumer; typed APIs use explicit tags.

The canonical C header is `cpp/include/graphdb/CGraphDB.h`. The root package exports it through the C-only forwarding header/module map in `cpp/c_api`; no generated copy is maintained. `make c-header-check` and CI compile that public surface as an ordinary C11 consumer.

```c
GraphDBOpenResult graphdb_open_v2(const char *path, unsigned long long path_len,
    long long hot_ttl_ms, long long sweep_interval_ms, const GraphDBQueryOptions *options);
GraphDBString graphdb_execute_query_v2(GraphDBHandle *handle,
    const char *query, unsigned long long query_len,
    const char *params_json, unsigned long long params_len, const GraphDBQueryOptions *options);
```

Lengths count UTF-8 bytes; no terminator is required. Query/property strings preserve NUL; filesystem paths reject it. JSON parameters are flat ordinary JSON scalars: escape NUL as `\u0000`, use integer decimal tokens for Int64 and decimal/exponent tokens for Double. Raw NUL in a JSON document is invalid, including after an otherwise complete object. Query and parameter buffers are each limited to 1 MiB. A null parameter pointer with zero length means no bindings. Options may only tighten database ceilings.

V2 query success is `{"schemaVersion":2,"ok":true,"data":...,"receipt":...}`; the receipt is absent for read-only queries. Graph property scalars, including projected properties, are `{"type":"int","value":1}` (tags: null, bool, int, double, string). Record metadata remains ordinary JSON. Errors are `{"schemaVersion":2,"ok":false,"error":{"code":...,"message":...,"context":...}}`. Unsupported schema versions and malformed responses throw in Swift.

V2 open success has a handle and empty error buffer; failure has a null handle and a v2 error buffer. Every C response/error buffer is owned by the caller and must be freed exactly once with `graphdb_string_free`, including on decode failure. `{NULL,0}` indicates no response allocation; freeing it is safe. Swift copies the explicit byte count under a single deferred free.

Legacy C exports remain available with their existing JSON envelopes and C-string restrictions. Legacy success uses `ok/data`; legacy errors keep string `error`, `code` and `context`. Both query ABIs share a buffered execution path: v2 reserves 256 result-budget bytes for framing/receipt and legacy reserves 32 bytes. Both allocate the bounded C output buffer before execution; successful serialization does not allocate after commit. This is conservative accounting, not a total RSS limit. See [errors](errors.md).

## Async and lifecycle

`TraversalBuilder` is an immutable Sendable struct. Each `out`, `in`, `depth` or `limit` returns a new value; keep the returned value. `collect()` returns nodes, `collect(as:)` returns typed nodes, and `paths()` returns real paths. Copies can be configured independently.

Async query, mutation, traversal, checkpoint, rebuild, trim and lifecycle methods await native completion on a utility queue. Task cancellation signals an independently owned native token. Deadlines include time queued for the engine gate and Swift decoding; there is no detached task. Storage operations retain their database and request until completion. Gate admission is serialized, not FIFO; long snapshots block writers. Cancellation is cooperative: arbitrary snapshot callback code, a Foundation decoder invocation and an individual OS IO call cannot be interrupted.

`GraphRequest(timeoutMilliseconds:)` accepts 1...5000 ms, starting at construction. Use a fresh token for each synchronous `queryResult(..., request:)` or `readSnapshot(..., request:)`; another thread may call `cancel()` without taking the engine gate. Async methods create their own token using the configured/per-call minimum timeout. Cancellation before WAL commit applies no writes. Once append starts, completion returns the durable receipt or `commitOutcomeUnknown` even if the task was cancelled. Never blindly retry an ambiguous mutation.

`readSnapshot(options:_:)` has sync and async entry points. Its body is always synchronous and receives a non-Sendable `GraphSnapshot` with `node`, `queryResult` and generic `query`. It holds one engine view across all reads, with a shared deadline and cumulative query budgets. No writes, checkpoint, rebuild, database reentrancy, async work or cross-thread snapshot access is permitted. An escaped Swift snapshot throws `closed`; the C callback pointer is strictly borrowed. Keep the body short and never wait for another operation on the same database.

`close(timeoutMilliseconds:)` has sync and async throwing overloads. It stops admission, cancels/drains admitted operations, stops the worker, checkpoints and releases the directory lock. Repeated completed closes return the same success/error; failed checkpoints are reported and never retried by deinit. A drain timeout leaves the database closing and allows a close retry after the blocked callback/IO completes. Calls after close begins throw `closed`. Deinit remains best-effort synchronous cleanup; explicitly await close when errors and UI responsiveness matter.

`suspend(timeoutMilliseconds: 1000)` cancels/drains work, stops tiering and checkpoints while retaining directory ownership. Normal query admission throws `busy` until `resume()` succeeds; bounded pressure trim remains permitted while fully suspended and participates in close draining. A drain timeout leaves admission suspended; retry suspend or resume deliberately. Once a lifecycle checkpoint starts, it finishes its storage protocol; timeout is not a hard total-duration cap. `trimMemory(to: 0)` and async `handleMemoryWarning()` return `GraphTrimResult` (evicted count/bytes, pinned bytes and unmet bytes). They force a bounded trim independent of TTL. Metadata and pinned payloads may remain resident. The old synchronous, nonthrowing memory-warning helper is deprecated.

`@MainActor GraphLifecycle` provides ordered `.background`, `.foreground` and `.memoryPressure` events via `send`, which returns a task whose error the app must handle. On UIKit, background execution is requested before dispatch and ended on completion or expiration; expiration cancels the operation. Call it from the app owner rather than once per independent window. See [Apple lifecycle integration](swiftui-lifecycle.md) for compiled examples and deadline limits.

C callers use `graphdb_request_create/cancel/retain/release`, `graphdb_execute_request_v2`, `graphdb_read_snapshot_v2`, `graphdb_snapshot_query_v2`, `graphdb_close_v2`, `graphdb_suspend_v2`, `graphdb_resume_v2` and `graphdb_trim_memory_v2`. Every concurrent caller must own a live handle reference (`graphdb_retain` before handing it off; `graphdb_release` afterward). Explicit close keeps the caller's reference valid for idempotent close/status calls. Legacy `graphdb_close` closes and releases one reference. Never race a final release with acquiring a reference from a raw pointer. Legacy synchronous maintenance is lifetime-safe but may finish its existing bounded work before close drains it; use the request-bearing/query APIs for cooperative cancellation.

## Maintenance APIs

`backup(to:)` captures a verified snapshot while the handle remains open. Offline `GraphMaintenance` provides inspect, verify, rebuildIndexes, backup, restore and migrateV1 with typed reports/errors. See [maintenance](backup-restore.md).

## Native traversal results

C++ `traverse_out(from, edge_type, depth, limit, include_paths = false)` and `traverse_in(to, edge_type, depth, limit, include_paths = false)` return `Result<TraversalResult>`. Each optional `GraphPath` contains ordered nodes and actual edges. Existing `walk_out`/`walk_in` preserve node vectors. These methods enforce work/allocation and serialized-equivalent output limits. See [ordering and bounds](ngql-syntax.md#deterministic-traversal-and-paths).


## Bounded developer inspection

`graphdb_inspect_v2(handle, kind, cursor, cursor_len, limit, options, request)` returns a normal v2 envelope with `data: {kind, revision, items, nextCursor}`. Kinds are 0 (nodes), 1 (edges), 2 (declared node-property indexes); limits are 1...100. Pass an empty cursor for the first page. Treat returned cursors as opaque and use them only for the same open engine instance/kind; any committed write, close/reopen, or database reset invalidates them (`conflict`). A cursor from another handle is rejected even at the same committed revision. Each page holds the operation gate, obeys cancellation and query memory/work/output ceilings, and reads cold payloads through the normal checked storage path. This is a bounded metadata scan, not an unbounded export or a stable snapshot across pages. Node/edge property values retain v2 scalar tags; index items contain `label` and `property`. Revision is a decimal string.

Swift exposes `GraphDatabase.inspect(_:cursor:limit:options:request:)` with `GraphInspectionKind.nodes`, `.edges` and `.indexes`, returning `GraphQueryResult`. These calls share existing C response ownership and request lifetimes. The development HTTP adapter serves these pages as described in the [local runner guide](developer-tools.md).
