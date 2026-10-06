#include "graphdb/MaintenanceOperations.hpp"
#include <set>
namespace graphdb {
namespace {
constexpr size_t maximum = 64 * 1024 * 1024;
Status bad(std::string s, const std::filesystem::path &p = {}) {
  return Status::Error(std::move(s), ErrorCode::corruptData, {p.string()});
}
bool header(ByteReader &r, uint32_t want) {
  uint32_t magic, version;
  return r.read_u32(magic) && magic == want && r.read_u32(version) && version == 1;
}
bool checksum(const std::vector<uint8_t> &b) {
  if (b.size() < 4)
    return false;
  ByteReader r(b.data() + b.size() - 4, 4);
  uint32_t c;
  return r.read_u32(c) && c == crc32_compute(b.data(), b.size() - 4);
}
Result<GraphObject> record(const uint8_t *bytes, size_t size) {
  if (size < 4 || size > kMaxObjectBytes)
    return {bad("invalid legacy record extent"), {}};
  ByteReader crc(bytes + size - 4, 4);
  uint32_t expected;
  crc.read_u32(expected);
  if (expected != crc32_compute(bytes, size - 4))
    return {bad("legacy record checksum mismatch; no text fallback"), {}};
  ByteReader r(bytes, size - 4);
  GraphObject o;
  uint8_t kind;
  if (!header(r, 0x4E4F5641) || !r.read_u8(kind) || (kind != 1 && kind != 2) ||
      !r.read_i64(o.last_read_ms) || !r.read_i64(o.last_modified_ms) || !r.read_u64(o.version) ||
      !o.version || !r.read_string(o.id) || !r.read_string(o.label_or_type) ||
      !r.read_string(o.from) || !r.read_string(o.to) || !r.read_properties(o.properties) ||
      r.remaining())
    return {bad("invalid or unsupported legacy record"), {}};
  o.kind = ObjectKind(kind);
  try {
    validate_identity(o.id);
    validate_identity(o.label_or_type);
    validate_properties(o.properties);
    if (kind == 1) {
      if (!o.from.empty() || !o.to.empty())
        return {bad("legacy node has endpoints"), {}};
    } else {
      validate_identity(o.from);
      validate_identity(o.to);
      if (o.id != o.from + "->" + o.label_or_type + "->" + o.to)
        return {bad("legacy edge identity mismatch"), {}};
    }
  } catch (const std::exception &e) {
    return {bad(e.what()), {}};
  }
  return {Status::OK(), std::move(o)};
}
} // namespace
Result<CatalogData> read_legacy_v1(const std::filesystem::path &dir, std::shared_ptr<FileIO> io) {
  // Refuse histories where the delimiter ID denotes different tuples. Replaying
  // them into current tuple IDs would invent edges absent from the v1 graph.
  std::map<std::string, std::string> legacy_edges;
  auto unambiguous_edge = [&](const std::string &from, const std::string &type,
                              const std::string &to) {
    auto old = from + "->" + type + "->" + to;
    auto current = edge_id(from, type, to);
    auto [it, inserted] = legacy_edges.emplace(old, current);
    return inserted || it->second == current;
  };
  CatalogData data;
  data.database_id = parse_uuid(new_transaction_id());
  auto file = io->read(dir / "catalog.db", maximum);
  bool has_catalog = bool(file);
  if (!file && file.status.code != ErrorCode::notFound)
    return {file.status, {}};
  size_t total = 0;
  if (file) {
    if (!checksum(file.value))
      return {bad("legacy catalog checksum mismatch", dir / "catalog.db"), {}};
    ByteReader r(file.value.data(), file.value.size() - 4);
    uint32_t count;
    if (!header(r, 0x4E434154))
      return {
          Status::Error("only binary catalog v1 can be migrated", ErrorCode::unsupportedVersion),
          {}};
    if (!r.read_u64(data.last_lsn) || !r.read_u32(count) || count > r.remaining() / 8)
      return {bad("invalid legacy catalog header"), {}};
    std::set<std::pair<std::string, std::string>> definitions;
    for (uint32_t i = 0; i < count; ++i) {
      std::string a, b;
      if (!r.read_string(a) || !r.read_string(b))
        return {bad("invalid legacy declaration"), {}};
      try {
        validate_identity(a);
        validate_identity(b);
      } catch (...) {
        return {bad("invalid legacy declaration"), {}};
      }
      if (!definitions.emplace(a, b).second)
        return {bad("duplicate legacy index"), {}};
      data.declared_indexes.emplace_back(a, b);
    }
    if (!r.read_u32(count) || count > r.remaining() / 13)
      return {bad("invalid legacy record count"), {}};
    std::set<std::string> keys;
    for (uint32_t i = 0; i < count; ++i) {
      std::string key, disk;
      uint8_t tier;
      uint32_t n;
      if (!r.read_string(key) || !r.read_u8(tier) || (tier != 1 && tier != 2) ||
          !r.read_string(disk) || !r.read_u32(n) || !r.has_bytes(n))
        return {bad("invalid legacy manifest"), {}};
      auto o = record(file.value.data() + r.position(), n);
      if (!o)
        return {o.status, {}};
      r.skip(n);
      if (key != object_key(o.value.kind, o.value.id) || !keys.insert(key).second)
        return {bad("duplicate or mismatched legacy identity"), {}};
      if (tier == 2) {
        if (disk.empty() || disk.size() > 255 || disk.find('\0') != std::string::npos ||
            std::filesystem::path(disk).filename() != disk || disk == "." || disk == "..")
          return {bad("unsafe legacy payload filename"), {}};
        auto payload = io->read(dir / "cold" / disk, kMaxObjectBytes);
        if (!payload)
          return {payload.status, {}};
        auto full = record(payload.value.data(), payload.value.size());
        if (!full)
          return {full.status, {}};
        const auto &a = o.value;
        const auto &b = full.value;
        // v1 increments the manifest version immediately after writing a cold payload.
        if (a.kind != b.kind || a.id != b.id || a.label_or_type != b.label_or_type ||
            a.from != b.from || a.to != b.to || b.version == UINT64_MAX ||
            a.version != b.version + 1 || a.last_modified_ms != b.last_modified_ms ||
            !a.properties.empty())
          return {bad("legacy payload does not match its manifest", dir / "cold" / disk), {}};
        o.value.properties = std::move(full.value.properties);
      }
      if (o.value.kind == ObjectKind::Edge) {
        if (!unambiguous_edge(o.value.from, o.value.label_or_type, o.value.to))
          return {bad("ambiguous legacy delimiter identity"), {}};
        o.value.id = edge_id(o.value.from, o.value.label_or_type, o.value.to);
      }
      auto bytes = serialize_object_binary(o.value).size();
      if (bytes > maximum - total)
        return {Status::Error("legacy snapshot exceeds 64 MiB", ErrorCode::limitExceeded), {}};
      total += bytes;
      StoredRecord rec;
      rec.object = std::move(o.value);
      data.records.push_back(std::move(rec));
    }
    if (r.remaining())
      return {bad("trailing legacy catalog bytes"), {}};
  }
  MemoryHotStore hot;
  hot.load_records(data.records, data.declared_indexes);
  data.indexes = hot.dump_indexes();
  auto wal = io->read(dir / "wal.log", maximum);
  if (!wal)
    return {bad("legacy WAL is missing or unreadable; complete history cannot be established",
                dir / "wal.log"),
            {}};
  ByteReader r(wal.value.data(), wal.value.size());
  if (!header(r, 0x4E57414C))
    return {Status::Error("only binary WAL v1 can be migrated", ErrorCode::unsupportedVersion), {}};
  std::vector<RecoveredTransaction> groups;
  uint64_t previous = 0;
  bool first = true;
  while (r.remaining()) {
    uint32_t n, crc;
    uint64_t lsn;
    uint8_t op;
    if (!r.read_u32(n) || n < 9 || n > kMaxBatchBytes || !r.has_bytes(size_t(n) + 4))
      return {bad("legacy WAL is truncated; no provable transaction tail repair"), {}};
    auto bytes = wal.value.data() + r.position();
    ByteReader f(bytes, n);
    r.skip(n);
    r.read_u32(crc);
    if (crc != crc32_compute(bytes, n) || !f.read_u64(lsn) || !lsn || !f.read_u8(op) || op < 1 ||
        op > 6 || (!first && (previous == UINT64_MAX || lsn != previous + 1)))
      return {bad("legacy WAL checksum/op/LSN mismatch"), {}};
    if (first && lsn != 1 &&
        (!has_catalog || data.last_lsn == UINT64_MAX || lsn != data.last_lsn + 1 || op != 6))
      return {bad("legacy WAL lacks checkpoint coverage"), {}};
    first = false;
    previous = lsn;
    Mutation m;
    bool valid = false;
    switch (op) {
    case 1: {
      UpsertNode x;
      valid = f.read_string(x.label) && f.read_string(x.id) && f.read_properties(x.properties);
      x.modified_ms = 0;
      m = std::move(x);
      break;
    }
    case 2: {
      UpsertEdge x;
      valid = f.read_string(x.type) && f.read_string(x.from) && f.read_string(x.to) &&
              f.read_properties(x.properties);
      x.modified_ms = 0;
      m = std::move(x);
      break;
    }
    case 3: {
      DeleteNode x;
      valid = f.read_string(x.id);
      m = std::move(x);
      break;
    }
    case 4: {
      DeleteEdge x;
      valid = f.read_string(x.from) && f.read_string(x.type) && f.read_string(x.to);
      m = std::move(x);
      break;
    }
    case 5: {
      CreateIndex x;
      valid = f.read_string(x.label) && f.read_string(x.property);
      m = std::move(x);
      break;
    }
    case 6: {
      uint64_t checkpoint;
      valid =
          f.read_u64(checkpoint) && has_catalog && checkpoint <= data.last_lsn && checkpoint < lsn;
      break;
    }
    }
    if (!valid || f.remaining())
      return {bad("invalid legacy WAL mutation"), {}};
    if (op == 2) {
      const auto &edge = std::get<UpsertEdge>(m);
      if (!unambiguous_edge(edge.from, edge.type, edge.to))
        return {bad("ambiguous legacy edge history; explicit application reconciliation required"),
                {}};
    } else if (op == 4) {
      const auto &edge = std::get<DeleteEdge>(m);
      if (!unambiguous_edge(edge.from, edge.type, edge.to))
        return {bad("ambiguous legacy edge history; explicit application reconciliation required"),
                {}};
    }
    if (lsn > data.last_lsn && op != 6)
      groups.push_back({{new_transaction_id(), {std::move(m)}}, lsn});
  }
  if (!has_catalog && first)
    return {bad("empty legacy history has no catalog"), {}};
  auto result = MaintenanceAccess::replay(std::move(data), groups, true);
  if (!result)
    return result;
  // Start new history; legacy LSNs never become a current-format recovery chain.
  result.value.last_lsn = 0;
  result.value.generation = 1;
  return result;
}
} // namespace graphdb
