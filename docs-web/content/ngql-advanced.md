# Parameters, Projections & Explain

NGQL supports parameter substitution, comparisons, projections and basic explain output. See the [implemented grammar and request budgets](ngql-syntax.md).

<!-- snippet: swift-parameters -->

Typed `queryResult` and Codable `query(_:parameters:as:)` accept `[String: GraphValue]`; `rawQuery` retains scalar `[String: Any]` compatibility. Native binding preserves equals signs, newlines, quotes and comment markers without reparsing values as query text. Size parameters must be nonnegative integers. Bindings preserve Int64 and Double types; nested values and nonfinite numbers throw.

<!-- snippet: ngql-advanced -->

Projection selects fields and scalar properties, not expressions. NGQL `walk ... paths` returns real shortest-hop paths; projections apply to every object in its envelope. Swift `paths()` decodes the actual ordered nodes and edges. Explain reports breadth-first traversal, canonical-edge tie-breaking, direction/access path and node/path mode; it does not provide measured execution costs. See [path semantics](ngql-syntax.md#deterministic-traversal-and-paths).

The static browser playground shows read-only recordings from executable engine examples. Use the [local developer query server](developer-tools.md) to exercise the actual engine.
