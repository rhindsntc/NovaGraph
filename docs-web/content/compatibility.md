# Compatibility and migration

The current engine uses schema-3 CURRENT roots, catalogs and records, with version-4 WAL segments (family 2, capabilities 3). Binary v1 conversion is implemented as an explicit read-only import into a new directory. Earlier FORMAT markers, foundation format 2 and intermediate WAL3 development directories remain unsupported. No version is silently upgraded or reset in place.

Recognizable v1 `catalog.db`/`wal.log` directories return `unsupportedVersion` from `graphdb_open_result`, with migration instructions, before a lock file is added. Unrecognized nonempty directories without CURRENT fail with `corruptData`; unknown current component versions fail with `unsupportedVersion`. See [open and ownership](errors.md).

| Source | Ordinary open | Conversion |
|---|---|---|
| CURRENT3 / catalog3 / record3 / WAL4 | Validate and replay | Backup/restore to a new directory |
| Binary catalog1 / record1 / WAL1 | Reject with migration instructions | Explicit `migrate-v1` |
| Legacy text payloads | Reject | Unsupported; no binary-error fallback |
| Foundation2 / intermediate WAL3 / unknown future format | Reject | No importer shipped |

## Import a stopped v1 database

```sh
make cpp-build
build/cpp/nova-maintenance inspect /path/to/legacy
build/cpp/nova-maintenance migrate-v1 /path/to/legacy /path/to/new-database
build/cpp/nova-maintenance verify /path/to/new-database
```

The destination must be absent, outside the source, and have an existing parent. Stop every old writer before inspecting/importing: the old engine cannot honor current ownership locks. Migration adds no files to the source and never repairs/truncates it. The checked-in fixture tests compare every source file byte before/after migration.

The importer validates bounded counts, checksums, enum values, identities, scalar tags, record versions and complete consecutive WAL entries. It loads every catalog-referenced cold payload and accounts for the v1 manifest version increment during cold transition. Edge IDs are re-encoded from endpoints/type, and indexes are rebuilt from authoritative properties. Complete post-checkpoint WAL mutations replay in memory using v1 semantics: recorded deletes of absent objects are no-ops, and edges may precede their nodes when the final graph has all endpoints. Histories in which one legacy delimiter ID maps to different edge tuples are rejected for explicit application reconciliation. Missing payloads, mismatched manifests, duplicate identities, dangling endpoints, damaged/truncated WAL records and unsupported encodings fail the operation with no published destination. There is no automatic salvage that silently skips bad records.

The original v1 format lacked atomic transaction groups and dependable durability. Migration can preserve only validated bytes that still exist; it cannot prove historical acknowledgments, recover already-lost properties, detect every previously overwritten delimiter collision, or recover a batch's missing operations. Stored timestamps retain their original clock basis. v1 WAL upserts did not store modification timestamps, so imported WAL-only upserts use zero to mean unknown and the report includes a warning. A new database UUID and fresh current-format LSN history distinguish the imported database.

A failed operation can leave an abandoned sibling staging directory after process termination. A publication error after rename may leave a complete new destination; validate it before retrying. See [backup, restore and repair](backup-restore.md) for ordering, error handling, the 64 MiB snapshot limit, C/Swift wrappers and tested commands.

Existing C pointer-open functions return null on failure; `graphdb_open_result` retains typed errors and buffer ownership. Existing query errors remain unchanged. The versioned API provides length-bearing C calls, typed Swift bindings/results/errors/receipts and raw-method deprecations while retaining legacy exports and raw String return shapes. The traversal builder is now a value type; assign the value returned by configuration methods. Maintenance retains its typed reports/errors. Root source SwiftPM integration is implemented; Apple runtime qualification remains open. Artifact tooling can build the Apple slice matrix. Nova stays experimental until release evidence matches the shipped revision.

## Source package and runtime qualification

The root manifest declares Swift tools 6.0 and C++20. `GraphDBKit` is the consumer product/module, with the C-only boundary hiding private C++ and JSON headers. See [installation](quickstart.md).

| Platform | Declared source minimum | Qualification status |
|---|---|---|
| macOS | 13 | Development examples and sample checks run on recent macOS; oldest runtime not qualified |
| iOS | 16 | Simulator checks and manual iPhone 17 Pro Max / iOS 27.0 sample smoke pass; full device and oldest-runtime qualification open |
| tvOS | 16 | Declared; runtime qualification incomplete |
| watchOS | 9 | Declared; runtime qualification incomplete |
| Mac Catalyst | 16 | Declared; runtime qualification incomplete |
| visionOS | None | Unsupported |

On 2026-10-06, a maintainer reported a passing manual sample run on **iPhone 17 Pro Max / iOS 27.0**, built from local source revision `e42262f` in **Debug**. Checks covered graph creation/editing, paths, NGQL reads, persistence across close/reopen and app launches, background/return, and reads after manual trim. See the [physical-device smoke record](../../examples/NovaGraphApp/README.md#physical-device-smoke-test). This establishes that tested combination; minimum runtimes, device workload budgets, binary consumers and broader platform qualification remain open.

Successful deployment-target compilation is not execution on that OS. Artifact tooling includes Apple architecture slices, but binary deployment floors can vary by architecture and SDK: watchOS arm64 requires 26 with the current SDK, while arm64_32 retains 9. Native Linux CMake testing is separate from Swift package and Apple runtime qualification. No Linux Swift package support claim follows from native tests.

No hosted package URL, published release tag or verified binary download is promised by this guide. Local Git URL/candidate-tag acceptance tests isolate source, build and dependency caches; they do not prove hosted installation.

## Product limitations

NovaGraph is experimental and not production-ready. Use disposable data for evaluation. API and on-disk compatibility may change; never assume an unsupported format will be upgraded in place.

- One process owns a database directory at a time. There is no distributed or simultaneous cross-process access.
- Properties are flat scalar values; nested Codable payloads are rejected.
- Requests have bounded depth, work, results and workspace. Large operations can fail before mutation with `limitExceeded`.
- Cancellation and deadlines are cooperative. A blocking OS call or callback cannot be preempted; a commit already in its durable protocol must finish safely.
- Engine accounting is not a process RSS guarantee. Physical-device performance, sustained regression baselines and full platform qualification remain incomplete.
- The static playground displays recorded fixtures. Real execution requires an explicit connection to the local developer runner.
- Fault campaigns are finite. Their passing does not establish production durability across all filesystems and hardware.

Read [errors](errors.md), [recovery](recovery.md) and [backup/restore](backup-restore.md) before designing retry or recovery behavior.

## Release evidence

The [local release preflight](release-preflight.md) records the candidate revision, actual component versions, C-header/export fingerprint, declared platform wording, documentation hashes and supplied evidence/artifact hashes. Matching fingerprints do not certify ABI compatibility, a declared target does not prove runtime support, and a passing source consumer does not qualify a binary download. Complete quality, platform and final release review remain required; the initial preflight retains a no-go decision.
