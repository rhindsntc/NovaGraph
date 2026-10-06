# NGQL syntax and bounded execution

NGQL is Nova's embedded query language. It supports strict parsing, source diagnostics, request budgets, deterministic shortest-hop paths, typed Swift responses, cancellable requests, closure snapshots and explicit close. Nova remains experimental pending platform and release qualification.

## Supported grammar

Keywords are case-insensitive. Property names and identities retain their case. Whitespace and `//` line comments are accepted outside quoted strings, including between tokens and after a final statement. Quote identities containing `//`, such as URLs. Semicolons split statements only outside strings/comments; empty separators are ignored. An empty/comment-only program is an error.

```text
program    := statement (";" statement)* [";"]
statement  := ["explain"] command
command    := ("create" | "upsert") "node" atom atom ["set" properties]
            | ("create" | "upsert") "edge" atom atom "->" atom ["set" properties]
            | "delete" "node" atom
            | "delete" "edge" atom atom "->" atom
            | "get" "node" atom ["return" projection]
            | "get" "edge" atom atom "->" atom ["return" projection]
            | "find" "nodes" atom find-clause*
            | "walk" "from" atom walk-clause*
            | "create" "index" "on" atom "(" atom ")"
            | "rebuild" ("index" | "indexes")
            | "checkpoint"
find-clause := "where" atom comparison value | "return" projection | "limit" size
walk-clause := "over" atom | "direction" ("out" | "in")
             | "depth" size | "limit" size | "paths" | "return" projection
properties := atom ("=" | ":") value ("," atom ("=" | ":") value)*
projection := atom ("," atom)* | "{" atom ("," atom)* "}"
comparison := "=" | "==" | "!=" | "<" | "<=" | ">" | ">="
```

Each find/walk clause can appear once, in any order. Empty projections/property lists, missing commas, trailing commas, duplicate properties/clauses, unknown directions and trailing syntax are errors. In colon-style assignments use `key: value`; colon can also occur inside an identity. `create node/edge` uses upsert semantics, replacing properties.

`atom` is an unquoted token, a single/double-quoted string or a `$name` parameter. Unquoted tokens stop at whitespace, grammar punctuation, arrows and comments. Strings support `\n`, `\r`, `\t`, `\\`, `\"` and `\'`; unsupported escapes are rejected. Values are flat strings, Bool, Int64, finite Double and null. Bare nonnumeric words are strings. See [scalar fidelity and identities](values.md) for persisted bounds.

`size` is an unquoted nonnegative decimal integer or a parameter bound to a nonnegative Int64. Negative numbers, `+1`, fractions, string-valued size parameters and overflow (including `18446744073709551616`) are rejected before conversion. Scalar identity parameters retain their canonical string conversion for compatibility; values bind directly and are never reparsed as query text. Quoted `'$name'` is literal text.

## Programs and parameters

```ngql
// This comment may contain a semicolon ;
upsert node Person ada set name="Ada", age=37;
upsert node Person grace set name="Grace";
upsert edge FOLLOWS ada -> grace;
find nodes Person where age >= $minimum return {id, name} limit $count;
walk from ada over FOLLOWS direction out depth 2 limit 25;
```

For the example, bind `minimum` and `count` as integers. Native callers use `PropertyMap`; C uses a flat JSON object; Swift typed calls use `[String: GraphValue]`; compatibility raw calls also accept scalar `[String: Any]`. Equals signs, newlines, quotes, semicolons and comment markers in bound strings retain their value. Nested JSON values are rejected. Typed Swift bindings are available through `queryResult` and `mutate`.

The whole program is parsed and bound before execution. Mutating programs run against staged state and publish one durable transaction only after every statement, query budget and output check succeeds. A failed statement or precommit limit/cancellation publishes no logical writes. Maintenance commands (`checkpoint`, `rebuild`) cannot be mixed with logical mutations. Reads may update cache/activity metadata. Multi-statement responses contain `results`; one statement returns its own result.

## Defaults and ceilings

| Budget | Default and supported maximum |
|---|---:|
| Traversal depth | 16 |
| Requested results per find/walk | 10,000 |
| Expanded edges across a program | 100,000 |
| Accounted query working allocation | 16 MiB |
| Serialized NGQL result payload | 16 MiB |
| Cooperative query timeout | 5,000 ms |
| Statements per program | 1,000 |
| Accounted staged mutation data | 16 MiB |
| NGQL text / parameter transport | 1 MiB each |

A walk without a depth clause requests 1; find/walk without a limit request 100. Those implicit requests are clamped to lower configured ceilings. Explicit requests above a ceiling fail with `limitExceeded`. Zero depth/results/expanded-edge budgets are allowed restrictions; byte, statement and timeout settings must be positive. Unsupported higher settings are `invalidArgument`.

Index queries use ordered posting cursors and read one candidate at a time, rather than sorting/copying an entire bucket. Traversal accounts for every examined edge, including duplicate destinations, before resolving it. Queues, seen sets, token storage, bound values, graph staging, record copies and JSON growth are charged before growth. The working budget is a conservative cumulative allocation allowance for the request, not measured live heap or RSS; a long scan can exhaust it even when earlier temporaries have been freed. Mutation bytes similarly include conservative staging overhead. Projection can reduce output but does not avoid loading its source record. Limits fail the whole request without returning partial data as success.

Serialized payload accounting includes multi-statement framing and escaped text. The C success/error transport envelope is separate. Errors remain available even when a success result cannot fit. The existing engine memory budgets may further reduce query allowances.

Deadline/cancellation checks run during parsing, cursor iteration, traversal and serialization, and before durable publication. Operation-gate waits poll the independent token/deadline every two milliseconds; Swift Task cancellation signals that token and decoding checks it too. Individual blocking file operations and user snapshot callbacks cannot be interrupted: expiry is checked when control returns. Once durable commit starts, the existing commit protocol completes and returns its receipt or ambiguous error. This is not a hard wall-clock deadline guarantee.

## Configuration

C++ uses `EngineConfig::query_options` (`QueryOptions`). C callers start with `graphdb_default_query_options()` and pass a lowered configuration to `graphdb_open_with_options_result`. Legacy opens use the same native defaults. Swift obtains its defaults from that C API:

```swift
var configuration = GraphDatabaseConfiguration(path: databaseURL)
configuration.queryOptions.maxDepth = 4
configuration.queryOptions.maxResults = 500
configuration.queryOptions.maxExpandedEdges = 10_000
configuration.queryOptions.workingBytes = 8 * 1024 * 1024
configuration.queryOptions.resultBytes = 2 * 1024 * 1024
configuration.queryOptions.timeoutMilliseconds = 1_000
let db = try GraphDatabase(configuration: configuration)
```

Options are validated before opening. Request-level native contexts can only tighten the database settings. `configs/nova.yaml` is explicitly illustrative; neither the engine nor the developer CLI loads it.

## Diagnostics and remaining work

Native `Status` and C error JSON preserve typed codes such as `parseError`, `unboundParameter`, `limitExceeded`, `cancelled`, `deadlineExceeded` and `notFound`. Query context includes a one-based `statementIndex` plus zero-based UTF-8 byte offsets `sourceStart` and `sourceEnd` (end-exclusive). Syntax errors point to the offending token, lexical errors to the consumed invalid token, and execution/resource errors to their statement. Input-wide failures may have no statement span. Offsets include leading whitespace/comments from the original input. Swift's compatibility query API still presents an error message; typed calls preserve structured diagnostic models.

`get node missing; walk from a depth -1` fails while parsing statement 2, before reading statement 1, with byte span `[36, 38)`. See [error contracts](errors.md).

`return` selects object fields/properties; it is not an expression language. Explain reports implemented access paths, not measured costs. Patterns, aliases, `merge/from` syntax and all-path enumeration are future work, not accepted grammar.

## Deterministic traversal and paths

`walk` without a `paths` clause still returns an array of reachable node objects (or projected objects). Add the bare `paths` clause to return `{"nodes":[...],"paths":[{"nodes":[...],"edges":[...]}]}`. The arrays align: path `i` ends at result node `i`. `return paths` remains a property projection; it does not enable path mode. `return {id}` with the `paths` clause projects every node and edge in the envelope. Omit the projection for full objects, including edge properties and endpoints.

<!-- snippet: ngql-traversal-paths -->

This executable example returns Bob, Cyra and Dina, using IDs `bob`, `cyra`, `dina`. Dina's selected route is `ada → bob → dina`, with the actual `KNOWS` edges and their properties. The native acceptance suite executes this source file through the C API. Typed Swift `Path` decoding and builder integration preserve the same ordered nodes and edges.

Traversal uses breadth-first search. It excludes the starting node, discovers each reachable node once and selects one shortest-hop path per result. A self-loop or cycle never adds the start back. `depth 0`, `limit 0` and an absent start produce empty arrays. Depth counts edges, and limit counts unique existing destination nodes, not adjacency candidates. Reaching the requested result limit returns a deterministic prefix. Exceeding an execution budget is a typed failure with no partial success.

Each parent's adjacency is visited in ascending canonical edge-key byte order; the breadth-first queue preserves that first-discovery order across parents. Equal-hop ties select the first discovery. The canonical key is `e/` plus the length-prefixed `(from,type,to)` tuple (decimal UTF-8 byte lengths); this is deterministic, not locale or plain-ID alphabetical sorting. The selection/order is stable across insertion order, cold tiering, rebuild and reopen of the same logical graph. Read-activity timestamps can change between calls and are not part of that guarantee.

For `direction in`, path nodes start at the requested node and follow incoming edges backwards. Edge `from`/`to` retain stored direction: `edge[i].to == node[i].id`, `edge[i].from == node[i+1].id`. Outbound paths use the opposite equalities. Every full path has one more node than edge. Default mutation validation requires both endpoints. In explicit legacy permissive mode, missing endpoints are skipped; they cannot bridge routes through nonexistent nodes. Storage errors are propagated, never treated as missing endpoints.

The engine caches resolved records for one traversal so shared path prefixes do not reread cold payloads. This cache ends with that traversal, including between statements of an NGQL program; it is not a cross-query cache. Reads still use the existing promotion/residency rules. Predecessors, cache entries, copied path objects and JSON output are charged to request budgets before growth. Projections reduce serialized output, but the executor still resolves full records.

Work is proportional to examined adjacency candidates with ordered cursor lookups (logarithmic in a posting bucket), plus the sum of selected path lengths for reconstruction. Cache space is proportional to resolved records, predecessor space to discovered nodes, and materialized paths can use `O(results × depth)` object copies. Node-only mode omits those path copies. Every examined edge, including loops and duplicate routes, consumes the expanded-edge allowance. No all-path enumeration or cross-traversal performance guarantee is provided.

`explain walk ... paths` reports `algorithm: BreadthFirst`, the actual inbound/outbound adjacency access path, `result_mode: paths`, `tie_break: canonical_edge_key`, and `path_semantics: one_shortest_hop`, along with depth/limit. Explain does not traverse data or estimate costs.

Explicit NGQL `checkpoint` is subject to query workspace and result limits. Swift synchronous/async `checkpoint()` and C maintenance checkpoint APIs instead share the close/suspend maintenance policy; see [checkpoint APIs](maintenance.md#checkpoint-apis).
