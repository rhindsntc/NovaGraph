# Transaction write-ahead log

NovaGraph implements version-4 transaction frames in the custom WAL. Every individual native mutation uses a one-operation WAL transaction. The engine stages complete mutating programs and publishes them atomically after sync, with retained catalog generations selected by CURRENT.

The segment header and begin/mutation/commit encodings follow the [format contract](file-format.md). Each transaction has a 128-bit identity, consecutive LSNs, checked frame lengths, per-frame CRCs and a final transaction CRC. A batch has 1–1,000 operations and at most 16 MiB of encoded mutation data. A receipt contains the transaction ID and commit LSN and is returned only after file sync succeeds. Each active segment is bounded to 64 MiB; checkpoint rotates to a fresh durable segment and retains history for both catalog generations.

```text
validate and allocate encoded batch
          |
          v
begin -> mutations -> commit frame -> durable file sync -> receipt
          |                                 |
    incomplete terminal tail          write/sync failure
          |                                 |
validate whole history                outcome unknown; fence writes
          |
durable tail truncate + sync
          |
       append allowed
```

Recovery first validates without writing. Only complete committed groups are returned. A short terminal frame or uncommitted terminal transaction may be removed after all prior history validates. A complete frame with a bad checksum, invalid operation/count, wrong transaction ID or nonconsecutive LSN is corruption even at EOF. It is never silently discarded. Tail repair truncates only the permitted suffix and syncs before another append; failure fences the WAL.

A write or sync failure can leave a committed transaction on disk even when no receipt reached the caller. `commitOutcomeUnknown` includes its transaction ID, blocks further writes/checkpoints and requires reopen. There is no automatic retry or idempotency promise. A definite capacity rejection occurs before writing and leaves checkpoint available.

`nova_wal` tests complete groups, every byte truncation of a small transaction, frame boundaries of a larger batch, structural/header/checksum corruption, failed sync/repair, batch bounds and LSN continuity across segments. Recovery campaigns test the integrated graph against process termination and modeled power loss.

Upsert mutations carry the exact wall-clock modification timestamp captured before staging/logging, including zero and negative values. Version 4 rejects the earlier timestamp-free version-3 WAL.

C/Swift opens now require the schema-3 CURRENT generation root referencing version-4 WAL segments. Preserve unsupported foundation and intermediate-format directories for application-managed recovery; no in-place upgrade occurs.
