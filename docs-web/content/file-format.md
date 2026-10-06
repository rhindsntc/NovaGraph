# Nova storage format contract

Status: implemented experimental format with transaction WAL framing, binary catalogs and payloads, CURRENT generation roots and explicit backup/restore/repair/import. No existing database may be silently upgraded in place.

## Encoding and bounds

All integers are little-endian with fixed widths; strings are a u32 byte length followed by validated UTF-8. Int64 uses two's-complement bits, Double IEEE-754 binary64 bits, Bool only 0 or 1. Scalar tags are null=0, bool=1, int64=2, double=3, string=4. Signed zero survives storage and JSON transport; numeric equality considers both zeros equal. Reject NaN and infinity.

Default hard format limits: 1,024 UTF-8 bytes per identity component, 1,024 per property key, 1 MiB per string/parameter input, 4,096 properties per object, 16 MiB per encoded object and 64 MiB per WAL frame. Validate counts against remaining bytes and format limits before reserve/allocation. Every decoder must consume its exact declared extent, reject duplicate property names and invalid enums, and reject unsupported versions distinctly from corruption.

Logical node identity is its ID. Edge identity is the ordered tuple `(from, type, to)`, encoded as decimal byte length, colon, bytes for each component. Index compound keys use the same unambiguous tuple representation. Strings, Bool, null and numbers occupy distinct key domains. Integral finite Double within Int64 range shares its equality key with that Int64; other doubles use a round-trip representation. Comparisons must not round an Int64 through Double.

## File envelopes

Each immutable payload/catalog/root file has a four-byte magic, u32 component version, u64 payload-byte count, payload, and u32 CRC32 (IEEE polynomial) over header+payload. Appendable WAL segments are an exception: they have the fixed header and individually checksummed frames described below, with no changing whole-file length/checksum in their header. Component versions are independent and recorded by the database root. A readable header is not proof of a valid body. CRC detects damage, not adversarial tampering.

The root also carries database UUID, format family and required capabilities. A reader rejects unknown required capabilities. Prototype revisions may use an explicit foundation-format marker; that marker is incompatible with legacy v1 and does not claim the final generation format. Any incompatible implementation step increments its component version and preserves old data through explicit import only.

## Immutable payloads and catalogs

Payload filenames are bounded opaque random names allocated exclusively; user identities never become filenames. Each record stores kind, canonical identity, label/type, endpoints, logical version, wall-clock metadata and tagged properties. A catalog reference stores payload filename, identity, logical version, byte length and checksum. Validate every field on promotion. Live monotonic inactivity clocks are not persisted across boots.

Catalog payload order: database UUID; generation u64; committed LSN u64; object count u32; length-delimited object manifests; declared-index count u32 and definitions; then four length-delimited sections (label, property, outbound, inbound). Each section carries kind u8, version u32, byte extent u64, entry count u32, key/posting entries and CRC32. Descriptor CRC32 is the referenced file’s envelope checksum (header+payload, excluding its own checksum trailer), rather than the constant CRC residue of an entire CRC-appended file. Posting entries contain canonical object identity and logical version, never memory pointers. Keys and postings are sorted for reproducibility and duplicate detection. Entries must refer to existing compatible manifests. Rebuild constructs a separate replacement and publishes only after validation.

CURRENT overview (schematic, use the normative table below for byte order): database identity, format family/capabilities and generation; current+previous catalog descriptors; active WAL descriptor; retained WAL descriptor array. Immutable catalog and sealed WAL descriptors carry bounded filename, component version, final byte length/CRC32, generation and LSN range. The active WAL descriptor instead carries only filename, version, database UUID, segment/predecessor IDs, immutable header CRC32 and first LSN; validated frames determine its changing tail. Sealing freezes and syncs a segment at a transaction boundary before a new CURRENT can publish its sealed descriptor and the already-synced new active header. CURRENT.tmp is never a recovery root. Atomic rename plus directory sync selects the new root; scanning for newest filenames is forbidden.

## WAL frames

WAL segment header: magic/version, database UUID (16 bytes), segment ID u64, predecessor ID u64, first LSN u64, header CRC32. Frame envelope: frame-body length u32, LSN u64, kind u8, transaction UUID (16 bytes), payload, CRC32 over length+LSN+kind+UUID+payload. Kinds are begin, mutation, commit. Begin declares mutation count and total staged bytes; mutation encodes a tagged complete logical operation; commit repeats count and includes a digest/CRC over all transaction frames in order. No nested transactions or interleaving within a serialized writer.

The commit LSN identifies the entire group. Recovery validates transaction identity, declared sizes, counts and complete framing before applying anything. Checkpoint descriptors record committed LSN, not an allocated/uncommitted sequence number. See [transition rules](transactions.md) for legal tail repair and retention.

## Normative production encoding table

All fields are packed without padding, in the listed order. UUID is 16 opaque bytes. `str` is u32 length + UTF-8 bytes. `blob` is u32 length + exactly that many bytes. Counts are u32 unless explicitly listed otherwise. Optional fields use a u8 presence flag restricted to 0/1 followed by the value when 1. CRC32 is the IEEE algorithm used by `crc32_compute`, initial and final XOR 0xffffffff. Record, catalog and CURRENT schemas use version 3. WAL uses component version 4: it adds persisted upsert modification timestamps and explicitly rejects the incompatible intermediate version-3 WAL. Version 2 remains an unsupported foundation prototype.

| Structure | Ordered fields |
|---|---|
| immutable envelope | magic u32; version u32=3; payloadLength u64; payload; CRC32 u32 covering every preceding byte |
| file magic | record 0x4E4F5641; catalog 0x4E434154; CURRENT 0x4E435552; WAL 0x4E57414C |
| immutable record payload | kind u8 (node=1/edge=2); id str; labelOrType str; from str; to str; logicalVersion u64; modifiedUnixMillis i64; properties |
| properties | count u32; repeated key str + scalar tag u8 + value (null none, bool u8, int i64, double binary64, string str); keys unique and sorted by UTF-8 bytes |
| catalog descriptor | filename str; version u32; byteLength u64; fileCRC32 u32; generation u64; committedLSN u64 |
| sealed WAL descriptor | filename str; version u32; byteLength u64; fileCRC32 u32; segmentID u64; predecessorID u64; firstLSN u64; lastCommittedLSN u64 |
| active WAL descriptor | filename str; version u32; UUID; segmentID u64; predecessorID u64; headerCRC32 u32; firstLSN u64 |
| CURRENT payload | UUID; family u32=2; requiredCapabilities u64; generation u64; currentCatalog descriptor; optional previousCatalog descriptor; activeWAL descriptor; sealedCount u32; repeated sealedWAL descriptor |
| WAL header | magic u32; version u32=4; UUID; segmentID u64; predecessorID u64; firstLSN u64; CRC32 u32 over preceding header bytes |
| begin payload | mutationCount u32; stagedBytes u64 |
| mutation payload | operation u8 (upsertNode=1, upsertEdge=2, deleteNode=3, deleteEdge=4, createIndex=5); operation fields below |
| upsertNode fields | label str; id str; properties; modifiedUnixMillis i64 |
| upsertEdge fields | type str; from str; to str; properties; modifiedUnixMillis i64 |
| deleteNode fields | id str |
| deleteEdge fields | from str; type str; to str |
| createIndex fields | label str; property str |
| commit payload | mutationCount u32; transactionCRC32 u32 over complete begin+mutation frames including their frame CRCs, in order; commit frame excluded |
| manifest | recordKind u8; canonicalID str; labelOrType str; from str; to str; logicalVersion u64; storageTag u8 (inline=0, cold=1); inline: record blob; cold: filename str + byteLength u64 + recordCRC32 u32 |
| index entry | canonicalKey str; postingCount u32; repeated canonicalObjectID str + logicalVersion u64 |

A WAL frame's u32 length counts LSN, kind, UUID and payload, excluding the length and final CRC fields. Frame kinds are begin=1, mutation=2, commit=3. Begin declares the canonical encoded mutation payload byte sum as stagedBytes; actual reserved working memory is metered separately. All frames including begin/commit consume consecutive LSNs. Empty transactions are rejected.

A new empty database starts with generation 1, an empty catalog at committedLSN 0, no previous catalog, no sealed WAL, and active segment 1/predecessor 0/firstLSN 1. Sync its files and directory before publishing CURRENT. A missing CURRENT in a nonempty initialized directory is corruption, not permission to bootstrap over it. Capabilities bit 0 means framed transactions and bit 1 means persisted indexes; production requires both. Unknown required bits fail open. Backup pins the root and referenced files until its copy is verified; GC cannot collect those references.
