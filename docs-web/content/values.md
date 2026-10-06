# Values and record identity

The native scalar codec implements the contract described here. These guarantees apply to native property maps, JSON parameter transport and binary records. GraphDBKit extends scalar fidelity to Swift typed bindings, reads and flat Codable models. The v2 C wire format tags graph property scalars so Int64, integral Double and Bool stay distinct.

| Value | Native contract |
|---|---|
| Int64 | Exact signed 64-bit value, including both bounds and values above 2^53 |
| Double | Finite IEEE-754 value; round-trip JSON precision, signed zero and double type retained |
| Bool | Distinct from numeric 0/1 |
| Null | Distinct null value |
| String | Valid UTF-8, including Unicode, quotes, backslashes, newlines, equals, commas and embedded NUL |

Properties form a flat object. Arrays, nested objects, duplicate JSON keys, malformed UTF-8/JSON, nonfinite doubles and out-of-range numbers are rejected. nlohmann/json 3.12.0 is pinned privately in C++; its MIT license is bundled. Parameter strings use JSON escapes, so `\u0000` reaches the native string without truncating the legacy C input. Literal NUL terminates legacy C strings. JavaScript Number values are not a safe way to construct Int64 JSON; produce the exact decimal text or use a native encoder.

## Limits

| Field | Bound |
|---|---|
| Node ID, label, edge endpoint or type | 1–1024 UTF-8 bytes each |
| Property key | At most 1024 UTF-8 bytes |
| Property string | At most 1 MiB |
| Properties per object | At most 4096 |
| C query or parameter JSON | At most 1 MiB each |
| Encoded binary object | At most 16 MiB; property validation also bounds escaped JSON size |
| Foundation catalog or WAL file | At most 64 MiB; later generations/segments are still planned |

Upserting a node replaces its properties under its ID. Upserting an edge replaces its properties under `(from, type, to)`. The edge ID is a length-prefixed UTF-8 tuple, not an arrow-delimited string. For `a`, `E`, `b`, the internal value is `1:a1:E1:b`. Numeric parameters used as IDs use the same round-trip conversion (a Double 1.0 becomes `1.0`). Prefer explicit string IDs. Treat this representation as internal; use endpoint/type APIs. Components containing `->`, separators or NUL cannot collide. Cold filenames are short generated names, independent of identifiers. Edge endpoints must exist in staged state by default; the native-only permissive mode is explicit.

Numeric comparisons and equality-index keys agree: integer 1 equals double 1.0, but neither equals Bool true. Mixed Int64/Double comparisons do not first round the integer to Double. Signed zero compares equal to zero. Cold indexes persist and rebuild from authoritative records under the current storage protocol.

## Executable fidelity example

<!-- snippet: value-fidelity -->

This source is compiled and executed by `cpp.documentation_value_fidelity_example` in both native build modes. See [executable documentation](documentation.md) for other examples.

Native property names are byte-distinct. Raw Swift payload JSON retains both composed and decomposed Unicode keys. Typed results reject duplicate or canonically equivalent keys that a Swift String-keyed dictionary cannot represent, with `GraphDBError.decodingFailed`; they never silently discard one.
