#include "graphdb/Catalog.hpp"
#include <charconv>
#include <set>

namespace graphdb {
namespace {
constexpr uint32_t magic = 0x4E434154;
constexpr size_t max_catalog = 64 * 1024 * 1024;
Status bad(const char *s) { return Status::Error(s, ErrorCode::corruptData); }
void blob(std::vector<uint8_t> &out, const std::vector<uint8_t> &bytes) {
  encode_u32(out, bytes.size());
  out.insert(out.end(), bytes.begin(), bytes.end());
}
bool read_blob(ByteReader &r, const uint8_t *base, std::span<const uint8_t> &out) {
  uint32_t n;
  if (!r.read_u32(n) || !r.has_bytes(n))
    return false;
  out = {base + r.position(), n};
  return r.skip(n);
}
Status validate(const CatalogData &d) {
  std::map<std::string, const StoredRecord *> records;
  for (const auto &rec : d.records) {
    const auto &o = rec.object;
    try {
      validate_identity(o.label_or_type);
      if (o.kind == ObjectKind::Node) {
        validate_identity(o.id);
        if (!o.from.empty() || !o.to.empty())
          return bad("node has endpoints");
      } else if (o.kind != ObjectKind::Edge || o.id != edge_id(o.from, o.label_or_type, o.to))
        return bad("invalid object identity");
    } catch (const std::exception &e) {
      return Status::Error(e.what(), ErrorCode::corruptData);
    }
    if (!o.version || !records.emplace(object_key(o.kind, o.id), &rec).second)
      return bad("duplicate identity or invalid version");
    if (rec.tier == StorageTier::Cold &&
        (rec.disk_key.empty() || rec.disk_key.size() > 255 ||
         rec.disk_key.find('\0') != std::string::npos ||
         std::filesystem::path(rec.disk_key).filename() != rec.disk_key || rec.disk_key == "." ||
         rec.disk_key == ".." || rec.payload_bytes < 20 || rec.payload_bytes > kMaxObjectBytes))
      return bad("invalid cold payload reference");
  }
  std::set<std::pair<std::string, std::string>> declared;
  for (const auto &def : d.declared_indexes) {
    try {
      validate_identity(def.first);
      validate_identity(def.second);
    } catch (...) {
      return bad("invalid index declaration");
    }
    if (!declared.insert(def).second)
      return bad("duplicate index declaration");
  }
  MemoryHotStore expected;
  expected.load_records(d.records, d.declared_indexes);
  auto known = expected.dump_indexes();
  for (size_t i = 0; i < 4; ++i) {
    if (i != 1 && known.sections[i] != d.indexes.sections[i])
      return bad("index does not match record manifests");
    for (const auto &[key, postings] : d.indexes.sections[i]) {
      if (postings.empty())
        return bad("empty index posting list");
      for (const auto &[id, version] : postings) {
        auto found = records.find(id);
        if (found == records.end() || found->second->object.version != version)
          return bad("index references missing or stale object");
        if (i == 1) {
          const auto &rec = *found->second;
          if (rec.object.kind != ObjectKind::Node)
            return bad("property index references edge");
          bool matches = false;
          for (const auto &[label, prop] : declared)
            if (label == rec.object.label_or_type) {
              const auto prefix = tuple_key({label, prop});
              if (key.starts_with(prefix)) {
                if (rec.tier == StorageTier::Cold) {
                  auto suffix = std::string_view(key).substr(prefix.size());
                  auto colon = suffix.find(':');
                  size_t length = 0;
                  if (colon == std::string_view::npos)
                    continue;
                  auto parsed = std::from_chars(suffix.data(), suffix.data() + colon, length);
                  if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + colon ||
                      suffix.substr(0, colon) != std::to_string(length) ||
                      length != suffix.size() - colon - 1 || !length)
                    continue;
                  auto encoded = suffix.substr(colon + 1);
                  try {
                    Property value;
                    if (encoded[0] == 'n' || encoded[0] == 'd')
                      value = parse_scalar(std::string(encoded.substr(1)));
                    else
                      value = parse_scalar(std::string(encoded.substr(1)));
                    if (property_equality_key(value) != encoded)
                      continue;
                  } catch (...) {
                    continue;
                  }
                  matches = true;
                  break;
                }
                auto value = rec.object.properties.find(prop);
                if (value != rec.object.properties.end() &&
                    key == tuple_key({label, prop, property_equality_key(value->second)})) {
                  matches = true;
                  break;
                }
              }
            }
          if (!matches)
            return bad("invalid property index key");
        }
      }
    }
  }
  for (const auto &[key, postings] : known.sections[1])
    for (const auto &[id, version] : postings) {
      auto k = d.indexes.sections[1].find(key);
      if (k == d.indexes.sections[1].end() || !k->second.count(id))
        return bad("missing property index posting");
    }
  return Status::OK();
}
} // namespace
Status Catalog::save(const std::filesystem::path &path, uint64_t lsn,
                     const std::vector<StoredRecord> &records,
                     const std::vector<std::pair<std::string, std::string>> &declared,
                     std::shared_ptr<FileIO> io) {
  CatalogData d;
  d.last_lsn = lsn;
  d.records = records;
  d.declared_indexes = declared;
  MemoryHotStore hot;
  hot.load_records(records, declared);
  d.indexes = hot.dump_indexes();
  return save(path, d, std::move(io));
}
Status Catalog::save(const std::filesystem::path &path, const CatalogData &d,
                     std::shared_ptr<FileIO> io) {
  auto valid = validate(d);
  if (!valid.ok)
    return valid;
  std::vector<uint8_t> p(d.database_id.begin(), d.database_id.end());
  encode_u64(p, d.generation);
  encode_u64(p, d.last_lsn);
  encode_u32(p, d.records.size());
  for (const auto &rec : d.records) {
    const auto &o = rec.object;
    std::vector<uint8_t> m;
    encode_u8(m, uint8_t(o.kind));
    encode_string(m, o.id);
    encode_string(m, o.label_or_type);
    encode_string(m, o.from);
    encode_string(m, o.to);
    encode_u64(m, o.version);
    encode_u8(m, rec.tier == StorageTier::Cold ? 1 : 0);
    if (rec.tier == StorageTier::Cold) {
      encode_string(m, rec.disk_key);
      encode_u64(m, rec.payload_bytes);
      encode_u32(m, rec.payload_crc);
    } else
      blob(m, serialize_object_binary(o));
    blob(p, m);
  }
  auto declared = d.declared_indexes;
  std::sort(declared.begin(), declared.end());
  encode_u32(p, declared.size());
  for (const auto &[label, prop] : declared) {
    encode_string(p, label);
    encode_string(p, prop);
  }
  for (size_t i = 0; i < 4; ++i) {
    std::vector<uint8_t> content;
    encode_u32(content, d.indexes.sections[i].size());
    for (const auto &[key, postings] : d.indexes.sections[i]) {
      encode_string(content, key);
      encode_u32(content, postings.size());
      for (const auto &[id, version] : postings) {
        encode_string(content, id);
        encode_u64(content, version);
      }
    }
    std::vector<uint8_t> section;
    encode_u8(section, i + 1);
    encode_u32(section, 1);
    encode_u64(section, content.size());
    section.insert(section.end(), content.begin(), content.end());
    encode_u32(section, crc32_compute(section.data(), section.size()));
    blob(p, section);
  }
  if (p.size() + 20 > max_catalog)
    return Status::Error("catalog exceeds 64 MiB", ErrorCode::limitExceeded);
  return atomic_write(*io, path, encode_envelope(magic, p));
}
Result<CatalogData> Catalog::load(const std::filesystem::path &path, std::shared_ptr<FileIO> io,
                                  CatalogReadMode mode) {
  auto file = io->read(path, max_catalog);
  if (!file)
    return {file.status, {}};
  auto envelope = decode_envelope(file.value.data(), file.value.size(), magic, max_catalog);
  if (!envelope)
    return {envelope.status, {}};
  const auto bytes = envelope.value;
  ByteReader r(bytes.data(), bytes.size());
  CatalogData d;
  uint32_t count;
  for (auto &byte : d.database_id)
    if (!r.read_u8(byte))
      return {bad("truncated catalog identity"), {}};
  if (!r.read_u64(d.generation) || !d.generation || !r.read_u64(d.last_lsn) || !r.read_u32(count) ||
      count > r.remaining() / 4)
    return {bad("invalid catalog header"), {}};
  for (uint32_t i = 0; i < count; ++i) {
    std::span<const uint8_t> manifest;
    if (!read_blob(r, bytes.data(), manifest))
      return {bad("invalid manifest extent"), {}};
    ByteReader m(manifest.data(), manifest.size());
    StoredRecord rec;
    auto &o = rec.object;
    uint8_t kind, tier;
    if (!m.read_u8(kind) || !m.read_string(o.id) || !m.read_string(o.label_or_type) ||
        !m.read_string(o.from) || !m.read_string(o.to) || !m.read_u64(o.version) ||
        !m.read_u8(tier) || tier > 1)
      return {bad("invalid manifest"), {}};
    o.kind = static_cast<ObjectKind>(kind);
    if (tier == 1) {
      rec.tier = StorageTier::Cold;
      if (!m.read_string(rec.disk_key) || !m.read_u64(rec.payload_bytes) ||
          !m.read_u32(rec.payload_crc))
        return {bad("invalid payload descriptor"), {}};
    } else {
      std::span<const uint8_t> object;
      if (!read_blob(m, manifest.data(), object))
        return {bad("invalid inline record extent"), {}};
      auto decoded = deserialize_object_binary(object.data(), object.size());
      if (!decoded)
        return {decoded.status, {}};
      const auto &value = decoded.value;
      if (value.kind != o.kind || value.id != o.id || value.label_or_type != o.label_or_type ||
          value.from != o.from || value.to != o.to || value.version != o.version)
        return {bad("inline record identity mismatch"), {}};
      o = std::move(decoded.value);
    }
    if (m.remaining())
      return {bad("trailing manifest bytes"), {}};
    d.records.push_back(std::move(rec));
  }
  if (!r.read_u32(count) || count > r.remaining() / 8)
    return {bad("invalid declaration count"), {}};
  for (uint32_t i = 0; i < count; ++i) {
    std::string label, prop;
    if (!r.read_string(label) || !r.read_string(prop))
      return {bad("invalid index declaration"), {}};
    d.declared_indexes.emplace_back(std::move(label), std::move(prop));
  }
  for (size_t i = 0; i < 4; ++i) {
    std::span<const uint8_t> section;
    if (!read_blob(r, bytes.data(), section) || section.size() < 21)
      return {bad("invalid index section extent"), {}};
    ByteReader s(section.data(), section.size());
    uint8_t kind;
    uint32_t version, crc;
    uint64_t extent;
    if (!s.read_u8(kind) || kind != i + 1 || !s.read_u32(version))
      return {bad("invalid index section kind"), {}};
    if (version != 1)
      return {Status::Error("unsupported index version", ErrorCode::unsupportedVersion), {}};
    if (mode == CatalogReadMode::rebuild_derived)
      continue;
    if (!s.read_u64(extent) || extent != section.size() - 17 || !s.read_u32(count) ||
        count > s.remaining() / 8)
      return {bad("invalid index extent/count"), {}};
    std::string previous;
    for (uint32_t j = 0; j < count; ++j) {
      std::string key;
      uint32_t n;
      if (!s.read_string(key, kMaxObjectBytes) || (j && key <= previous) || !s.read_u32(n) || !n ||
          n > s.remaining() / 12)
        return {bad("invalid index key/posting count"), {}};
      previous = key;
      auto &postings = d.indexes.sections[i][key];
      std::string prior;
      for (uint32_t k = 0; k < n; ++k) {
        std::string id;
        uint64_t v;
        if (!s.read_string(id) || !s.read_u64(v) || (k && id <= prior))
          return {bad("invalid posting"), {}};
        prior = id;
        postings.emplace(std::move(id), v);
      }
    }
    if (!s.read_u32(crc) || s.remaining() ||
        crc != crc32_compute(section.data(), section.size() - 4))
      return {bad("invalid index checksum"), {}};
  }
  if (r.remaining())
    return {bad("trailing catalog bytes"), {}};
  if (mode == CatalogReadMode::rebuild_derived) {
    MemoryHotStore rebuilt;
    rebuilt.load_records(d.records, d.declared_indexes);
    d.indexes = rebuilt.dump_indexes();
  }
  auto valid = validate(d);
  if (!valid.ok)
    return {valid, {}};
  return {Status::OK(), std::move(d)};
}
} // namespace graphdb
