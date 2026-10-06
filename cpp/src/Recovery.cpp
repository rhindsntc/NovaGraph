#include "graphdb/Recovery.hpp"
#include <set>
#include <algorithm>
namespace graphdb {
namespace {
constexpr uint32_t root_magic = 0x4E435552;
constexpr size_t max_file = 64 * 1024 * 1024;
// Match MemoryHotStore's object_key ordering without allocating encoded keys.
// Catalogs from other producers may be unordered; they retain full synchronization.
bool record_before(const StoredRecord &a, const StoredRecord &b) {
  if (a.object.kind != b.object.kind)
    return a.object.kind == ObjectKind::Edge;
  return a.object.id < b.object.id;
}
bool ordered_records(const std::vector<StoredRecord> &records) {
  return std::adjacent_find(records.begin(), records.end(), [](const auto &a, const auto &b) {
    return !record_before(a, b);
  }) == records.end();
}
bool same_cold_payload(const StoredRecord &a, const StoredRecord &b) {
  return a.tier == StorageTier::Cold && b.tier == StorageTier::Cold &&
         a.object.kind == b.object.kind && a.object.id == b.object.id &&
         a.object.label_or_type == b.object.label_or_type &&
         a.object.from == b.object.from && a.object.to == b.object.to &&
         a.object.version == b.object.version && a.disk_key == b.disk_key &&
         a.payload_bytes == b.payload_bytes && a.payload_crc == b.payload_crc;
}

Status corrupt(std::string message) {
  return Status::Error(std::move(message), ErrorCode::corruptData);
}
bool name_ok(const std::string &name) {
  return !name.empty() && name.size() <= 255 && name.find('\0') == std::string::npos &&
         std::filesystem::path(name).filename() == name && name != "." && name != "..";
}
void uuid(std::vector<uint8_t> &b, const UUID &id) { b.insert(b.end(), id.begin(), id.end()); }
bool uuid(ByteReader &r, UUID &id) {
  for (auto &v : id)
    if (!r.read_u8(v))
      return false;
  return true;
}
void catalog_descriptor(std::vector<uint8_t> &b, const CatalogDescriptor &d) {
  encode_string(b, d.filename);
  encode_u32(b, d.version);
  encode_u64(b, d.bytes);
  encode_u32(b, d.crc);
  encode_u64(b, d.generation);
  encode_u64(b, d.committed_lsn);
}
bool catalog_descriptor(ByteReader &r, CatalogDescriptor &d) {
  return r.read_string(d.filename) && name_ok(d.filename) && r.read_u32(d.version) &&
         r.read_u64(d.bytes) && d.bytes >= 20 && d.bytes <= max_file && r.read_u32(d.crc) &&
         r.read_u64(d.generation) && d.generation && r.read_u64(d.committed_lsn);
}
std::vector<uint8_t> encode_root(const DatabaseRoot &root) {
  std::vector<uint8_t> b;
  uuid(b, root.database_id);
  encode_u32(b, 2);
  encode_u64(b, 3);
  encode_u64(b, root.generation);
  catalog_descriptor(b, root.current);
  encode_u8(b, root.previous.has_value());
  if (root.previous)
    catalog_descriptor(b, *root.previous);
  const auto &a = root.active;
  encode_string(b, a.filename);
  encode_u32(b, a.version);
  uuid(b, a.database_id);
  encode_u64(b, a.segment);
  encode_u64(b, a.predecessor);
  encode_u32(b, a.crc);
  encode_u64(b, a.first_lsn);
  encode_u32(b, root.sealed.size());
  for (const auto &d : root.sealed) {
    encode_string(b, d.filename);
    encode_u32(b, d.version);
    encode_u64(b, d.bytes);
    encode_u32(b, d.crc);
    encode_u64(b, d.segment);
    encode_u64(b, d.predecessor);
    encode_u64(b, d.first_lsn);
    encode_u64(b, d.last_lsn);
  }
  return encode_envelope(root_magic, b);
}
Result<CatalogDescriptor> describe_catalog(const std::filesystem::path &dir,
                                           const std::string &name, const CatalogData &data,
                                           FileIO &io) {
  auto file = io.read(dir / name, max_file);
  if (!file)
    return {file.status, {}};
  return {Status::OK(),
          {name, 3, file.value.size(), crc32_compute(file.value.data(), file.value.size() - 4),
           data.generation, data.last_lsn}};
}
Result<WalDescriptor> describe_wal(const std::filesystem::path &dir, const std::string &name,
                                   const WalManager &wal, FileIO &io, bool sealed) {
  auto file = io.read(dir / name, max_file);
  if (!file)
    return {file.status, {}};
  if (file.value.size() < WalManager::header_bytes)
    return {corrupt("missing WAL header"), {}};
  WalDescriptor d;
  d.filename = name;
  d.database_id = wal.database_id();
  d.bytes = file.value.size();
  d.crc =
      crc32_compute(file.value.data(), sealed ? file.value.size() : WalManager::header_bytes - 4);
  d.segment = wal.segment_id();
  d.predecessor = wal.predecessor_id();
  d.first_lsn = wal.first_lsn();
  d.last_lsn = wal.committed_lsn();
  return {Status::OK(), std::move(d)};
}
Result<CatalogData> load_catalog(const std::filesystem::path &dir, const CatalogDescriptor &d,
                                 const UUID &id, std::shared_ptr<FileIO> io,
                                 CatalogReadMode mode = CatalogReadMode::strict) {
  if (d.version != 3)
    return {Status::Error("unsupported catalog descriptor version", ErrorCode::unsupportedVersion),
            {}};
  auto file = io->read(dir / d.filename, max_file);
  if (!file)
    return {file.status.code == ErrorCode::notFound ? corrupt("referenced catalog is missing")
                                                    : file.status,
            {}};
  if (file.value.size() != d.bytes || file.value.size() < 20 ||
      crc32_compute(file.value.data(), file.value.size() - 4) != d.crc)
    return {corrupt("catalog descriptor mismatch"), {}};
  auto loaded = Catalog::load(dir / d.filename, io, mode);
  if (!loaded)
    return loaded;
  if (loaded.value.database_id != id || loaded.value.generation != d.generation ||
      loaded.value.last_lsn != d.committed_lsn)
    return {corrupt("catalog identity/generation mismatch"), {}};
  return loaded;
}
} // namespace
Result<DatabaseRoot> Recovery::read_root(const std::filesystem::path &dir,
                                         std::shared_ptr<FileIO> io) {
  auto file = io->read(dir / "CURRENT", 1024 * 1024);
  if (!file)
    return {file.status, {}};
  auto bytes = decode_envelope(file.value.data(), file.value.size(), root_magic, 1024 * 1024);
  if (!bytes)
    return {bytes.status, {}};
  ByteReader r(bytes.value.data(), bytes.value.size());
  DatabaseRoot root;
  uint32_t family, count;
  uint64_t capabilities;
  uint8_t previous;
  if (!uuid(r, root.database_id) || !r.read_u32(family) || !r.read_u64(capabilities))
    return {corrupt("invalid root header"), {}};
  if (family != 2 || capabilities != 3)
    return {Status::Error("unsupported database capabilities", ErrorCode::unsupportedVersion), {}};
  if (!r.read_u64(root.generation) || !root.generation || !catalog_descriptor(r, root.current) ||
      !r.read_u8(previous) || previous > 1)
    return {corrupt("invalid root catalog"), {}};
  if (previous) {
    root.previous.emplace();
    if (!catalog_descriptor(r, *root.previous))
      return {corrupt("invalid previous catalog"), {}};
  }
  auto &a = root.active;
  if (!r.read_string(a.filename) || !name_ok(a.filename) || !r.read_u32(a.version) ||
      !uuid(r, a.database_id) || !r.read_u64(a.segment) || !r.read_u64(a.predecessor) ||
      !r.read_u32(a.crc) || !r.read_u64(a.first_lsn) || !r.read_u32(count) ||
      count > r.remaining() / 52)
    return {corrupt("invalid active WAL descriptor"), {}};
  for (uint32_t i = 0; i < count; ++i) {
    WalDescriptor d;
    d.database_id = root.database_id;
    if (!r.read_string(d.filename) || !name_ok(d.filename) || !r.read_u32(d.version) ||
        !r.read_u64(d.bytes) || d.bytes < WalManager::header_bytes || d.bytes > max_file ||
        !r.read_u32(d.crc) || !r.read_u64(d.segment) || !r.read_u64(d.predecessor) ||
        !r.read_u64(d.first_lsn) || !r.read_u64(d.last_lsn))
      return {corrupt("invalid sealed WAL descriptor"), {}};
    root.sealed.push_back(std::move(d));
  }
  if (r.remaining() || root.current.generation != root.generation ||
      a.database_id != root.database_id || a.first_lsn != root.current.committed_lsn + 1)
    return {corrupt("root generation/LSN mismatch"), {}};
  if (root.previous && (root.previous->generation >= root.generation ||
                        root.previous->committed_lsn > root.current.committed_lsn))
    return {corrupt("invalid retained catalog ordering"), {}};
  return {Status::OK(), std::move(root)};
}
Result<std::shared_ptr<Recovery>> Recovery::open(const std::filesystem::path &dir,
                                                 std::shared_ptr<FileIO> io, bool create,
                                                 CatalogReadMode mode) {
  auto self = std::shared_ptr<Recovery>(new Recovery(dir, io));
  auto loaded = read_root(dir, io);
  if (!loaded) {
    if (loaded.status.code != ErrorCode::notFound)
      return {loaded.status, {}};
    if (!create)
      return {loaded.status, {}};
    std::filesystem::create_directories(dir);
    auto contents = io->list(dir, 0, 2);
    if (!contents)
      return {contents.status, {}};
    for (const auto &entry : contents.value)
      if (entry != ".nova.lock")
        return {corrupt("CURRENT missing in nonempty database; explicit recovery/import required"),
                {}};
    auto &root = self->root_;
    root.database_id = parse_uuid(new_transaction_id());
    CatalogData empty;
    empty.database_id = root.database_id;
    auto catalog_name = "catalog-" + new_transaction_id() + ".db";
    auto saved = Catalog::save(dir / catalog_name, empty, io);
    if (!saved.ok)
      return {saved, {}};
    auto descriptor = describe_catalog(dir, catalog_name, empty, *io);
    if (!descriptor)
      return {descriptor.status, {}};
    root.current = descriptor.value;
    auto wal_name = "wal-" + new_transaction_id() + ".log";
    self->wal_ = std::make_shared<WalManager>(dir / wal_name, io);
    auto created = self->wal_->create(root.database_id, 1, 0, 1);
    if (!created.ok)
      return {created, {}};
    auto active = describe_wal(dir, wal_name, *self->wal_, *io, false);
    if (!active)
      return {active.status, {}};
    root.active = active.value;
    auto published = atomic_write(*io, dir / "CURRENT", encode_root(root));
    if (!published.ok)
      return {published, {}};
    self->catalog_ = std::move(empty);
    return {Status::OK(), std::move(self)};
  }
  self->root_ = std::move(loaded.value);
  auto &root = self->root_;
  auto catalog = load_catalog(dir, root.current, root.database_id, io, mode);
  if (!catalog)
    return {catalog.status, {}};
  self->catalog_ = std::move(catalog.value);
  if (root.previous) {
    auto prior = load_catalog(dir, *root.previous, root.database_id, io, mode);
    if (!prior)
      return {prior.status, {}};
  }
  std::set<std::string> names{root.current.filename};
  if (root.previous && !names.insert(root.previous->filename).second)
    return {corrupt("aliased catalog reference"), {}};
  uint64_t next = root.previous ? root.previous->committed_lsn + 1 : root.current.committed_lsn + 1;
  uint64_t preceding = 0;
  std::set<std::string> transactions;
  auto segments = root.sealed;
  segments.push_back(root.active);
  for (size_t i = 0; i < segments.size(); ++i) {
    const auto &descriptor = segments[i];
    bool active = i + 1 == segments.size();
    if (descriptor.version != WalManager::format_version)
      return {Status::Error("unsupported WAL descriptor version", ErrorCode::unsupportedVersion),
              {}};
    if (!names.insert(descriptor.filename).second || descriptor.first_lsn != next ||
        (i && descriptor.predecessor != preceding))
      return {corrupt("WAL reference chain mismatch"), {}};
    auto bytes = io->read(dir / descriptor.filename, max_file);
    if (!bytes)
      return {bytes.status.code == ErrorCode::notFound ? corrupt("referenced WAL is missing")
                                                       : bytes.status,
              {}};
    if (bytes.value.size() < WalManager::header_bytes ||
        (!active && bytes.value.size() != descriptor.bytes) ||
        crc32_compute(bytes.value.data(),
                      active ? WalManager::header_bytes - 4 : bytes.value.size()) != descriptor.crc)
      return {corrupt("WAL descriptor mismatch"), {}};
    auto wal = std::make_shared<WalManager>(dir / descriptor.filename, io);
    auto opened = wal->open();
    if (!opened.ok)
      return {opened, {}};
    if (wal->database_id() != root.database_id || wal->segment_id() != descriptor.segment ||
        wal->predecessor_id() != descriptor.predecessor || wal->first_lsn() != descriptor.first_lsn)
      return {corrupt("WAL header identity mismatch"), {}};
    auto replay = wal->recover_transactions();
    if (!replay)
      return {replay.status, {}};
    if (!active && (wal->needs_repair() || wal->committed_lsn() != descriptor.last_lsn))
      return {corrupt("sealed WAL is not a complete committed segment"), {}};
    for (auto &transaction : replay.value) {
      if (!transactions.insert(transaction.batch.transaction_id).second)
        return {corrupt("duplicate transaction across segments"), {}};
      if (transaction.committed_lsn > root.current.committed_lsn)
        self->transactions_.push_back(std::move(transaction));
    }
    next = wal->committed_lsn() + 1;
    preceding = descriptor.segment;
    if (active)
      self->wal_ = std::move(wal);
  }
  return {Status::OK(), std::move(self)};
}
Status Recovery::checkpoint(CatalogData data) {
  if (fenced_ || !wal_->writable())
    return Status::Error("checkpoint requires writable recovery state", ErrorCode::closed);
  if (root_.generation == UINT64_MAX || root_.active.segment == UINT64_MAX ||
      wal_->committed_lsn() == UINT64_MAX)
    return Status::Error("generation/LSN exhausted", ErrorCode::limitExceeded);
  data.database_id = root_.database_id;
  data.generation = root_.generation + 1;
  data.last_lsn = wal_->committed_lsn();
  // The selected catalog proves durability only for exactly matching immutable
  // payloads. Merge ordered manifests with constant scratch space; fall back to
  // full sync for other ordering/provenance. Failed attempts never update catalog_.
  const bool reuse = catalog_.database_id == root_.database_id &&
                     catalog_.generation == root_.current.generation &&
                     catalog_.last_lsn == root_.current.committed_lsn &&
                     ordered_records(catalog_.records) && ordered_records(data.records);
  auto prior = catalog_.records.begin();
  // New/uncertain references retain bounded per-file sync and shared Apple flush.
  // Every required group must finish before catalog publication.
  std::vector<std::filesystem::path> payloads;
  payloads.reserve(64);
  for (const auto &record : data.records) {
    if (record.tier != StorageTier::Cold)
      continue;
    if (reuse) {
      while (prior != catalog_.records.end() && record_before(*prior, record))
        ++prior;
      if (prior != catalog_.records.end() && same_cold_payload(*prior, record))
        continue;
    }
    payloads.push_back(directory_ / "cold" / record.disk_key);
    if (payloads.size() == 64) {
      auto synced = io_->sync_files(payloads);
      if (!synced.ok)
        return synced;
      payloads.clear();
    }
  }
  if (!payloads.empty()) {
    auto synced = io_->sync_files(payloads);
    if (!synced.ok)
      return synced;
  }
  auto name = "catalog-" + new_transaction_id() + ".db";
  auto saved = Catalog::save(directory_ / name, data, io_);
  if (!saved.ok)
    return saved;
  auto descriptor = describe_catalog(directory_, name, data, *io_);
  if (!descriptor)
    return descriptor.status;
  auto synced = io_->sync_file(directory_ / root_.active.filename);
  if (!synced.ok) {
    fenced_ = true;
    return synced;
  }
  auto sealed = describe_wal(directory_, root_.active.filename, *wal_, *io_, true);
  if (!sealed)
    return sealed.status;
  DatabaseRoot next = root_;
  next.generation = data.generation;
  next.previous = root_.current;
  next.current = std::move(descriptor.value);
  std::erase_if(next.sealed, [&](const auto &segment) {
    return segment.last_lsn <= next.previous->committed_lsn;
  });
  next.sealed.push_back(std::move(sealed.value));
  auto wal_name = "wal-" + new_transaction_id() + ".log";
  auto active = std::make_shared<WalManager>(directory_ / wal_name, io_);
  auto created = active->create(root_.database_id, root_.active.segment + 1, root_.active.segment,
                                data.last_lsn + 1);
  if (!created.ok)
    return created;
  auto active_descriptor = describe_wal(directory_, wal_name, *active, *io_, false);
  if (!active_descriptor)
    return active_descriptor.status;
  next.active = std::move(active_descriptor.value);
  auto bytes = encode_root(next);
  Status published;
  auto exception_failure =
      Status::Error("root publication interrupted; reopen required", ErrorCode::ioFailure);
  try {
    published = atomic_write(*io_, directory_ / "CURRENT", bytes);
  } catch (const FaultInterruption &) {
    fenced_ = true;
    throw;
  } catch (...) {
    fenced_ = true;
    return exception_failure;
  }
  if (!published.ok) {
    fenced_ = true;
    return published;
  }
  root_ = std::move(next);
  catalog_ = std::move(data);
  wal_ = std::move(active);
  transactions_.clear();
  io_->fault_point("wal.after_rotation");
  return Status::OK();
}
Status Recovery::install_snapshot(const std::filesystem::path &dir, CatalogData data,
                                  std::shared_ptr<FileIO> io) {
  if (data.last_lsn == UINT64_MAX || !data.generation)
    return Status::Error("snapshot generation/LSN exhausted", ErrorCode::limitExceeded);
  DatabaseRoot root;
  root.database_id = data.database_id;
  root.generation = data.generation;
  auto name = "catalog-" + new_transaction_id() + ".db";
  auto saved = Catalog::save(dir / name, data, io);
  if (!saved.ok)
    return saved;
  auto descriptor = describe_catalog(dir, name, data, *io);
  if (!descriptor)
    return descriptor.status;
  root.current = std::move(descriptor.value);
  auto wal_name = "wal-" + new_transaction_id() + ".log";
  WalManager wal(dir / wal_name, io);
  auto created = wal.create(root.database_id, 1, 0, data.last_lsn + 1);
  if (!created.ok)
    return created;
  auto active = describe_wal(dir, wal_name, wal, *io, false);
  if (!active)
    return active.status;
  root.active = std::move(active.value);
  return atomic_write(*io, dir / "CURRENT", encode_root(root));
}
} // namespace graphdb
