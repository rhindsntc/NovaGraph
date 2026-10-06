# Transactions, snapshots and checkpoints

A mutating NGQL program is one atomic transaction. The engine parses and binds every statement, validates against staged graph state, builds replacement graph/index state and produces query results before appending a transaction-framed WAL record. Successful sync precedes a nonthrowing state swap. A failed statement, binding, endpoint check, resource reservation or precommit cancellation leaves live data unchanged. Maintenance commands (`checkpoint`, `rebuild indexes`) cannot appear in a mutating program.

New edges require both endpoints in staged state. A batch can create nodes then their edge. Native `EngineConfig.legacy_permissive_endpoints` explicitly enables the old behavior; C/Swift opens use strict endpoints. Upserts replace properties and node deletion removes incident edges.

Native `commit_batch(batch, context)` returns a transaction ID and committed LSN. Cancellation is checked before commit; after append starts the bounded durable protocol finishes. A failed write/sync can mean `commitOutcomeUnknown`: preserve the transaction ID, reopen, inspect recovered application state and never blindly retry. Durable transaction-ID reconciliation tooling remains future work. IDs are not an unlimited idempotency service.

The engine serializes queries, writes, sweeps and checkpoints through one operation gate. Individual queries see a stable graph. Native `read_snapshot()` returns a noncopyable RAII handle holding that gate across reads; writers wait until it is destroyed. Keep the engine alive, use the handle on its creating thread, and keep snapshots short. Writes and reentrant engine calls from a snapshot are rejected. C borrowed callbacks and Swift `readSnapshot` closures provide the same stable view. Their body cannot await or call the database reentrantly. The entire snapshot shares one deadline and cumulative budgets. Gate waits poll cancellation/deadlines; a callback must return before close can release its resources. See [lifecycle APIs](graph-database.md#async-and-lifecycle). `transactionReceipt` and `mutate` expose typed durable receipts; Swift failure tests prove batches leave no partial writes.

```text
parse/bind → stage graph + indexes + results → append transaction → sync WAL
                                                               → swap state → acknowledge
```

`CURRENT` is the only recovery root. It names current and retained previous catalog generations and the WAL segments needed for both. Checkpoint captures one committed LSN under the gate, syncs cold references, writes a fresh catalog, syncs a new active WAL header, and atomically publishes a new root. The former active WAL is retained as a sealed segment. Production rotation never truncates a live WAL in place. Root publication failure fences writes and maintenance until reopen.

A missing or corrupt root or referenced catalog/WAL fails open without selecting the newest orphan. Recovery validates metadata and history in temporary state, then repairs only a provably incomplete terminal active-WAL suffix. No worker or usable handle is exposed before recovery succeeds. Normal open validates cold manifests without reading every payload; promotion validates the actual payload.

C/Swift handles always use managed roots and lifetime directory ownership. Direct native constructors accepting injected stores/WAL are low-level test facilities; use the managed Recovery path under `DatabaseLock` for durable embedding. Legacy v1 requires explicit import to a fresh destination; foundation/intermediate formats are unsupported. See [Offline maintenance](backup-restore.md).

Current checks cover batch rollback, parse/bind/maintenance rejection, staged endpoint validation, allocation failure, precommit cancellation, snapshot writer blocking, WAL-only replay, checkpoint/write interleavings and root-publication failure. Fault campaigns expand these into process-crash and modeled power-loss campaigns. These checks do not certify a production release.
