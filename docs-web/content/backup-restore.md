# Backup, restore and offline repair

NovaGraph provides the `nova-maintenance` CLI, native/C maintenance operations, `GraphDatabase.backup(to:)` and the offline `GraphMaintenance` Swift API. The custom engine remains experimental; these tools do not establish production readiness or platform certification.

## Build and inspect

```sh
make cpp-build
build/cpp/nova-maintenance inspect /path/to/database
build/cpp/nova-maintenance verify /path/to/database
```

`inspect` validates the recognized format and reports `format`, `generation`, `committedLSN`, `nodes`, `edges`, `indexes`, `tailRepairNeeded` and `warnings`. On current data it performs the same integrity checks as `verify`. Both read every referenced cold payload, compare derived indexes against authoritative values, check endpoints, validate both retained catalogs and their WAL coverage, then replay committed changes in memory. They do not repair the source WAL, create source payloads, or checkpoint the source. A repairable active tail is reported explicitly. Unknown versions and unprovable corruption return a structured error.

All offline commands require exclusive ownership of the source. Close current-format handles first; an active handle produces `busy`. Source paths must already exist. Read operations never initialize a missing database. Current-format ownership uses the existing lock and rejects a missing ownership file; restore/copy the complete directory instead of synthesizing an alternate lock. Legacy sources use a read-only advisory directory lock when no lock file exists. Stop every legacy writer yourself: v1 did not implement the ownership protocol. Never remove or replace an active lock file.

Successful CLI commands emit JSON with `ok: true` and exit 0. Failures emit `ok: false`, `code`, `error` and available file/OS context, and exit nonzero. Invalid CLI argument counts exit 2. Capture the result and check the exit status before using a destination.

## Create and restore a backup

```sh
build/cpp/nova-maintenance backup /path/to/database /backups/nova-2026-09-23
build/cpp/nova-maintenance verify /backups/nova-2026-09-23
build/cpp/nova-maintenance restore /backups/nova-2026-09-23 /path/to/restored-database
build/cpp/nova-maintenance verify /path/to/restored-database
```

The destination must be absent and its parent must exist. Existing destinations, aliases and destinations inside the source are rejected. Publication uses a platform no-replace directory rename, so a destination created concurrently is also preserved. Keep each backup at a distinct path; the tool never replaces the last good backup.

Backup captures one committed state, including complete WAL transactions after the catalog checkpoint. It materializes validated values, writes fresh immutable payloads and a single catalog generation with rebuilt/checked indexes, and starts an empty WAL at the captured committed LSN plus one. The database UUID, record identities, logical versions, scalar values and stored timestamps are preserved; catalog generation restarts at 1. The backup is a compacted logical snapshot, not a byte-for-byte forensic copy. Retired history and unrelated files are not copied. Restore uses the same validation and publication protocol; a restored database gets its own directory ownership even though the source UUID is retained.

Files are written and synced in a unique `.nova-stage-<id>` sibling directory. The complete staged database is verified before rename, then its parent directory is synced before success. A killed process can leave that staging directory; preserve it for diagnosis or remove only that abandoned stage after confirming no operation owns it. An error after rename can leave a complete destination despite a failed command: inspect/verify that destination before deciding how to proceed. Source data and other backups are never removed by cleanup.

Maintenance currently bounds the materialized snapshot to 64 MiB of encoded records. This is not an RSS bound; metadata and temporary index/replay allocations add overhead. Larger snapshots return `limitExceeded`. These are synchronous operations and can take time proportional to the full graph. Streaming backup, progress reporting and broader resource/cancellation controls are not implemented.

## Backup from an open Swift handle

```swift
try db.backup(to: backupURL)
let report = try GraphMaintenance.verify(backupURL)
let restored = try GraphMaintenance.restore(from: backupURL, to: restoredURL)
```

`backup(to:)` holds the engine operation gate through capture, writing and verification, so concurrent engine operations wait. It requires a healthy managed database. An uncertain/fenced handle must be reopened first. The offline `GraphMaintenance` methods are `inspect`, `verify`, `rebuildIndexes`, `backup(from:to:)`, `restore(from:to:)` and `migrateV1(from:to:)`. They return `GraphMaintenanceReport` and throw `GraphMaintenanceError` with a stable `code` and diagnostic `error` string. Only local file URLs are accepted. Typed query errors and cancellable request/lifecycle APIs are described in the [Swift API](graph-database.md).

C callers use `graphdb_backup(handle, destination)` or `graphdb_maintenance(command, source, destination)`, then free the returned buffer with `graphdb_string_free`. Native callers use `GraphEngine::backup` or `maintain`. Raw C paths end at NUL; Swift rejects embedded NULs before invoking C.

## Rebuild indexes explicitly

```sh
build/cpp/nova-maintenance rebuild-indexes /path/to/database
build/cpp/nova-maintenance verify /path/to/database
```

Repair validates root descriptors, catalog envelopes and authoritative manifests/payloads. It can discard malformed or inconsistent derived index contents within a validated known-format envelope and rebuild label, property and both adjacency indexes from authoritative records. It then replays validated committed transactions and atomically publishes a new complete generation. The old files remain until explicit GC determines they are no longer referenced.

Repair does not invent missing payloads, fix damaged record/catalog envelopes, accept future index versions, guess an orphan root, or roll back to an older catalog silently. Invalid authoritative data leaves the published root unchanged. If publication is interrupted, reopening observes the prior root or the complete replacement. Save a forensic copy before attempting recovery of valuable damaged data; use a verified backup when authoritative bytes cannot be validated.

## Evidence

The `nova_maintenance` suite exercises cold-index repair, missing payloads, active-WAL tails, source byte preservation, path aliases, destination races, live-handle backup and legacy conversion. It kills child processes at each instrumented IO/publication boundary in small backup, restore, migration and repair scenarios. The source and last complete backup survive; any published new destination validates as a whole snapshot. `tools/test-maintenance-cli.py` executes all six CLI commands against an independently encoded v1 fixture. Swift tests exercise the public maintenance workflow and typed busy/conflict errors. These are local POSIX process-crash tests; modeled power-loss campaigns remain separate checks and do not model maintenance directory publication.
