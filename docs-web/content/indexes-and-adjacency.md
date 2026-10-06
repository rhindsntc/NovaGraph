# Indexes & Adjacency

Catalog schema 3 persists label, declared property, outbound and inbound index sections. Postings carry canonical object keys and logical versions; reopen validates section checksums, ordering, references and manifest compatibility before loading them. Normal open does not read every cold payload. Cold manifests retain identity, label/type, endpoints and payload descriptors while unindexed property values remain on disk.

Declared property equality indexes preserve numeric equality between integral Double and Int64. Undeclared filters scan records and can promote cold payloads. Both traversal directions retain adjacency across tiering and restart.

`rebuildIndexes()` reads all authoritative records, including cold payloads, and constructs replacement indexes separately. It publishes only after every read and validation succeeds. A missing, corrupt or mismatched payload returns an error and preserves the existing index. Rebuild is a full data operation, with time and memory proportional to the graph. It is not a repair for missing authoritative data; preserve the directory and restore from a valid copy instead. [Offline maintenance](backup-restore.md) can rebuild derived indexes from validated records and publish a new generation.

Reads and residency changes do not increment logical record versions. Upserts do. Promotion checks the referenced identity, version, encoded length and checksum before replacing a cold manifest.

See [storage behavior](storage-engine.md), [format](file-format.md) and [limitations](compatibility.md). [Atomic publication and retained checkpoints](transactions.md) are implemented; broader platform and release qualification remain incomplete.
