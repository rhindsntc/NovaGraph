# Errors, directory ownership and durable I/O

Native failures use typed codes and additive C error transport. Swift query/open APIs throw `GraphCoreError` with `code`, `message` and optional typed `context`; only `notFound` maps to nil for point reads. Offline maintenance has typed reports/errors. The custom storage protocol remains experimental.

## Ownership and open

`graphdb_open_result(path, hot_ttl_ms, sweep_interval_ms)` returns `{handle, error}`. Success returns a non-null handle and an empty error buffer. Failure returns a null handle and a JSON error buffer. Free every returned buffer once with `graphdb_string_free`, including open errors. The older pointer-open functions return null and discard error details. `graphdb_close_v2(handle, request)` and Swift throwing `close` report checkpoint failure and retain the final result for idempotent calls. Legacy `graphdb_close` remains void and closes/releases one reference; it cannot report errors. This does not establish a production durability guarantee.

Each open database holds `.nova.lock` through admitted-operation drain and final checkpoint/cleanup. Explicit close releases directory ownership even while the closed handle object remains alive. Suspend retains ownership. Nova canonicalizes the directory and uses an exclusive nonblocking advisory OS lock. Same-process opens, symlink aliases and other processes conflict with `busy`; process death releases the lock. Never remove the lock file while a handle is active, bypass it with a different writer, or share a handle across `fork`.

New directories receive a schema-3 `CURRENT` root, initial empty catalog and active WAL. A nonempty directory without CURRENT, a damaged referenced catalog/WAL or invalid segment chain fails open without recreating missing history. Unknown versions/capabilities return `unsupportedVersion`. Failed initialization never checkpoints or replaces damaged evidence. Opening current data may create the advisory lock file. Recognizable binary v1 data is rejected before adding that file; use [explicit migration](compatibility.md).

## C error envelope

```json
{"ok":false,"error":"database directory already owned or lock unavailable","code":"busy","context":{"file":"/path/.nova.lock","osError":35,"object":"","transactionId":""}}
```

`osError` is a platform errno and varies across systems. `file`, `object` and `transactionId` are populated when available; the message remains separate from the stable code. Existing query consumers can keep reading `error`. All exports contain C++ exceptions. Allocation failure can return an empty buffer: check `data` before using it. Callers must still supply valid pointers, lengths implied by C strings and live handles.

| Code | Meaning / caller action |
|---|---|
| `ok` | Native operation succeeded |
| `notFound` | Requested logical record absent; generic FileIO also uses this for a missing path |
| `busy` | Another handle owns the directory, or this database is suspended; close the other owner or resume this database before retrying |
| `invalidArgument` | Invalid input, identity or parameter shape; correct the request |
| `parseError` | NGQL syntax/value parsing failed; correct the query |
| `unboundParameter` | Supply the named query parameter |
| `limitExceeded` | A bounded input/file exceeds capacity; reduce input or inspect capacity |
| `cancelled` | Cooperative request cancellation, including Swift propagation and cancellable gate waits |
| `deadlineExceeded` | Cooperative query deadline elapsed; blocking operations are checked when control returns |
| `ioFailure` | Inspect OS/file context, preserve data and resolve the I/O cause |
| `corruptData` | Invalid persisted bytes or a missing referenced payload; preserve evidence |
| `unsupportedVersion` | Use the matching build or explicit v1 importer when supported; do not overwrite |
| `closed` | WAL/handle writes are unavailable or fenced; resolve the cause and reopen |
| `conflict` | State changed or cursor invalidated; restart the affected operation |
| `commitOutcomeUnknown` | WAL write or sync failed; mutation may survive recovery. Do not blindly retry |

For example, `get node absent` returns `notFound`; `upsert node N a set v=$missing` returns `unboundParameter`; a second open of the same directory returns `busy`. Native scans and traversals return cold-read failures instead of silently omitting records. A missing referenced cold file is corruption, not an absent logical node.

## I/O foundation and its limits

`FileIO` supplies bounded reads, write-all, file sync, rename, directory sync, truncate and remove. The POSIX adapter retries interrupted operations and partial transfers, checks short reads, and preserves errno/path. Apple file sync uses `F_FULLFSYNC`; other POSIX platforms use `fsync`. Failed sync is an error, with no weaker silent fallback. Atomic replacement writes a temporary file, syncs it, renames it and syncs the parent directory. Cold payload writes sync both file and directory before returning a filename.

WAL append errors stop publication of that mutation and fence further WAL writes/checkpoints until reopen. Errors include a diagnostic `foundation-lsn-N` identity; this is not a public idempotency token. Recovery rejects complete damaged frames and durably repairs only validated incomplete terminal tails; see [transaction WAL](wal.md). The test-only I/O model separates volatile data, synced inode bytes and synced directory entries, including write success followed by sync failure and simulated power loss.

Atomic batches, serialized native snapshots and retained checkpoint generations are now implemented. Bounded garbage collection, native memory accounting and explicit throwing close are implemented; fault campaigns have finite coverage. The legacy sweep count also cannot distinguish all per-record failures. See [storage](storage-engine.md), [transaction semantics](transactions.md) and [testing](testing.md).

## Query diagnostics and options

Query errors include `statementIndex` (one-based), `sourceStart` and `sourceEnd` (zero-based UTF-8 byte offsets, end-exclusive) to query error context when a statement is available. The offsets refer to the original input, including comments and whitespace. Parse/bind errors identify the offending token; execution/resource failures identify the statement. Input-wide validation can omit these fields.

`graphdb_open_with_options_result` accepts validated `GraphDBQueryOptions`; start with `graphdb_default_query_options` and lower the desired ceilings. Swift `GraphDatabaseConfiguration.queryOptions` forwards the same settings. See the [grammar, limits and timing contract](ngql-syntax.md).

## Versioned C and Swift errors

The additive `graphdb_open_v2` and `graphdb_execute_query_v2` functions use explicit byte lengths and `schemaVersion: 2`. Their error member is an object containing `code`, `message` and `context`; legacy exports retain the envelope above. Swift typed calls reject unsupported versions and malformed responses rather than returning nil or an empty collection. `GraphCoreError.context` carries file/OS/record/transaction details and optional one-based statement index plus byte source offsets. Local Swift argument and decoding failures use `GraphDBError`; maintenance retains `GraphMaintenanceError`. See [API ownership and schemas](graph-database.md#raw-api-and-response-ownership).
