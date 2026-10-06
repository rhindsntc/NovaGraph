# Recovery and crash evidence

`CURRENT` is the sole publication root. It selects catalog generations and WAL segments; opening never guesses a newer orphan or silently creates empty history. The current catalog supplies durable label/property/adjacency indexes and cold manifests. Complete WAL transaction groups after its committed LSN replay into temporary state before publication. The previous catalog and its replay coverage remain retained for future explicit recovery tooling.

Recovery rejects invalid checksums, versions, enums, counts, identities, transaction digests and LSN chains. Only an incomplete terminal active-WAL frame or uncommitted terminal group with a validated prefix is eligible for durable truncation. A complete bad-CRC frame is corruption, including at EOF. Validation and allocation precede any permitted repair. Sealed segments cannot have repairable tails. Failed repair exposes no handle. Replay preserves logged modification timestamps and can spill validated state to immutable cold payloads to respect the configured hot-memory limit. A failed spill preserves the existing root and WAL evidence.

Normal catalog/index loading does not read every cold payload. Replaying an uncheckpointed index declaration can require authoritative cold reads. Each promotion validates the referenced identity, version, length and checksum. Missing or damaged referenced data fails the read/scan; it is never treated as an absent matching record. Offline maintenance provides a full offline integrity scan and explicit index repair; see [backup, restore and repair](backup-restore.md).

## Recovery memory

Open/recovery loads catalogs and replays WAL into temporary state before final residency checks and publication. Its temporary allocations can exceed the configured query and residency limits; there is no enforced aggregate recovery-workspace cap. Per-file validation limits and spilling recovered hot records do not establish a total process-memory ceiling.

Under the [first-release memory policy](maintenance.md#first-release-memory-policy), production support requires peak-process-memory evidence for checkpoint/reopen at each claimed workload and platform, plus sustained checks for growing memory use. These are qualification requirements, not completed qualification or runtime enforcement. Applications requiring a strict total-memory ceiling are outside that guarantee.

## Evidence and limits

The native `nova_recovery` suite exercises every instrumented boundary in a fixed small-graph scenario, root/payload truncation, retained-catalog corruption, unsupported versions and invalid index sections. WAL tests separately cut every byte of a small transaction and cover large frame boundaries, duplicate/nonconsecutive LSNs, count/digest mismatches, invalid tags and repair errors.

`tools/test-crash-recovery.py` runs two distinct campaigns:

- **Modeled power loss:** `ModelFileIO` keeps live and durable inode bytes and directory names separately. Power loss discards unsynced data and renames. Deterministic seeds vary scalar values and mutation order, with faults before/after IO and named publication phases. Each trace compares recovered state against externally retained acknowledgment state, checks whole node/edge updates, exact scalar values, indexed/scanned equivalence, adjacency and authoritative index rebuild, then verifies a new append and second recovery.
- **Process crash:** a child owns the database directory and receives SIGKILL at selected IO/publication boundaries. It never calls close or destructors. The parent records acknowledged epochs outside the database and syncs that evidence before reopening. Recovered epochs must equal the last acknowledgment or one whole in-flight transaction, with matching nodes, edge and indexes. This exercises actual POSIX operations and lock release; it does not simulate lost power or storage-controller behavior.

PR CI runs at least 1,000 modeled traces plus process traces; nightly CI runs 10,000 modeled traces. The process harness includes the first occurrence of every named point plus deterministic boundary samples, so its actual count can exceed the requested sample count. `cpp/tests/faults/seeds.json` retains the regression corpus. CI uploads machine-readable `recovery-evidence.json`, logs (including any failing seed/boundary) and the corpus. Reproduced failures must be added to the permanent corpus before a fix is accepted.

These finite tests cover small graphs and a particular filesystem model. They do not certify every filesystem, hardware power-loss behavior, high-volume performance or a production release. CI configuration alone is not evidence of a successful run on a platform.

## Handling an error

1. Stop writes after `commitOutcomeUnknown` or a fenced-handle error. Keep the transaction ID and application operation identity; do not blindly retry.
2. Close the unusable handle and reopen to validate/replay durable history. If open succeeds, inspect application state to resolve an ambiguous operation. Unlimited idempotency/reconciliation tooling is not implemented.
3. If open reports corruption or an unsupported version, preserve the complete directory. Do not delete CURRENT, choose a newer orphan catalog, truncate a checksummed frame manually, or overwrite the directory with an empty database.
4. Restore from a separately verified application copy if available. Use [Backup/restore and repair](backup-restore.md) or explicit [v1 import](compatibility.md); ordinary open does not perform these operations.
