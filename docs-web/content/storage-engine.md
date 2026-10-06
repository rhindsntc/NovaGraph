# Storage Engine and Durability

NovaGraph implements a custom embedded storage protocol. It remains experimental and has no verified production release.

## Current files and tiers

`MemoryHotStore` keeps records and indexes in memory. `FileDiskStore` writes immutable `.rec` cold payloads. A schema-3 `CURRENT` root selects schema-3 catalog generations and version-4 transaction-framed WAL segments. Catalogs retain record manifests plus label, property and inbound/outbound adjacency indexes. Binary records carry version headers and CRC32 checksums.

Ordinary open validates the selected root and history. It never silently resets or upgrades a database. See the [file format](file-format.md) and [compatibility boundary](compatibility.md).

## Inactivity sweep

The default hot threshold is 600 seconds, with a 30-second sweep interval. A cold candidate has a monotonic inactivity tick older than the TTL cutoff; persisted wall-clock metadata is separate. Payload writes are synced before residency changes. A read validates a cold payload and may promote it when the hot budget permits. Index entries survive tier transitions and reopen.

## Commit and recovery

Atomic batches stage changes, append a complete WAL transaction, sync it and then publish the new graph state. One operation gate serializes writes, queries, checkpoints and maintenance. Retained catalog generations and their WAL coverage support validated recovery. Storage errors propagate through native, C and typed Swift APIs; an absent referenced payload is an error, not an absent graph record.

A graceful close/reopen check does not prove crash recovery. The test suite includes separate process-crash and modeled power-loss campaigns with finite scope. Read [transactions](transactions.md), [WAL](wal.md), [recovery](recovery.md) and [errors](errors.md) before relying on acknowledgment and retry behavior.

## Maintenance APIs

Explicit throwing `close()` reports the final checkpoint outcome. `checkpoint()` publishes a retained generation. `rebuildIndexes()` constructs replacement indexes from authoritative records and preserves the old indexes on failure. `trimMemory()` and `handleMemoryWarning()` evict eligible payloads in bounded batches; pins, metadata and indexes can remain resident.

Offline tools provide integrity verification, derived-index repair, backup, restore and explicit legacy import. Garbage collection reclaims unreachable Nova files while preserving unknown external files. See [memory and maintenance](maintenance.md), [backup and restore](backup-restore.md) and [index behavior](indexes-and-adjacency.md).
