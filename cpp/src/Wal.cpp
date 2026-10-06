#include "graphdb/Wal.hpp"
#include <limits>
namespace graphdb {
namespace {
constexpr uint32_t magic = 0x4E57414C, version = WalManager::format_version;
constexpr size_t max_wal_bytes = 64 * 1024 * 1024;
std::vector<uint8_t> frame(uint64_t lsn, uint8_t kind, const UUID &id,
                           const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> bytes;
  encode_u32(bytes, uint32_t(25 + payload.size()));
  encode_u64(bytes, lsn);
  encode_u8(bytes, kind);
  bytes.insert(bytes.end(), id.begin(), id.end());
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  encode_u32(bytes, crc32_compute(bytes.data(), bytes.size()));
  return bytes;
}
bool read_uuid(ByteReader &r, UUID &id) {
  for (auto &byte : id)
    if (!r.read_u8(byte))
      return false;
  return true;
}
} // namespace
WalManager::WalManager(std::filesystem::path path, std::shared_ptr<FileIO> io)
    : path_(std::move(path)), io_(std::move(io)) {}
WalManager::~WalManager() { close(); }
std::vector<uint8_t> WalManager::make_header() const {
  std::vector<uint8_t> b;
  encode_u32(b, magic);
  encode_u32(b, version);
  b.insert(b.end(), database_id_.begin(), database_id_.end());
  encode_u64(b, segment_id_);
  encode_u64(b, predecessor_id_);
  encode_u64(b, first_lsn_);
  encode_u32(b, crc32_compute(b.data(), b.size()));
  return b;
}
Status WalManager::create(const UUID &id, uint64_t segment, uint64_t predecessor, uint64_t first) {
  std::lock_guard lock(mu_);
  database_id_ = id;
  segment_id_ = segment;
  predecessor_id_ = predecessor;
  first_lsn_ = first;
  auto header = make_header();
  auto status = io_->write_all(path_, header, WriteMode::exclusive);
  if (status.ok)
    status = io_->sync_file(path_);
  if (status.ok)
    status = io_->sync_directory(path_.parent_path());
  if (!status.ok)
    return status;
  byte_count_ = valid_bytes_ = header_bytes;
  next_lsn_ = first;
  committed_lsn_ = first - 1;
  opened_ = validated_ = true;
  return Status::OK();
}
Status WalManager::open() {
  std::lock_guard lock(mu_);
  opened_ = false;
  validated_ = false;
  failed_ = false;
  repair_needed_ = false;
  transaction_ids_.clear();
  auto data = io_->read(path_, max_wal_bytes);
  if (!data) {
    if (data.status.code != ErrorCode::notFound)
      return data.status;
    database_id_ = parse_uuid(new_transaction_id());
    std::filesystem::create_directories(path_.parent_path());
    auto status = atomic_write(*io_, path_, make_header());
    if (!status.ok)
      return status;
    byte_count_ = valid_bytes_ = header_bytes;
    next_lsn_ = first_lsn_;
    committed_lsn_ = first_lsn_ - 1;
    opened_ = validated_ = true;
    return Status::OK();
  }
  ByteReader r(data.value.data(), data.value.size());
  uint32_t m, v, crc;
  if (!r.read_u32(m) || m != magic || !r.read_u32(v))
    return Status::Error("invalid WAL header", ErrorCode::corruptData, {path_.string()});
  if (v != version)
    return Status::Error("unsupported WAL version", ErrorCode::unsupportedVersion,
                         {path_.string()});
  if (!read_uuid(r, database_id_) || !r.read_u64(segment_id_) || !r.read_u64(predecessor_id_) ||
      !r.read_u64(first_lsn_) || !r.read_u32(crc) || !first_lsn_ || !segment_id_ ||
      predecessor_id_ >= segment_id_ || crc != crc32_compute(data.value.data(), header_bytes - 4))
    return Status::Error("corrupt WAL header", ErrorCode::corruptData, {path_.string()});
  byte_count_ = data.value.size();
  opened_ = true;
  return Status::OK();
}
void WalManager::close() {
  std::lock_guard lock(mu_);
  opened_ = false;
}
Result<CommitReceipt> WalManager::append_transaction(const TransactionBatch &batch) {
  // Build all bytes and the receipt before starting durable IO.
  UUID id;
  std::vector<std::vector<uint8_t>> payloads;
  size_t staged_bytes = 0;
  try {
    id = parse_uuid(batch.transaction_id);
    if (batch.mutations.empty() || batch.mutations.size() > kMaxBatchStatements)
      return {Status::Error("batch count outside 1..1000", ErrorCode::limitExceeded), {}};
    payloads.reserve(batch.mutations.size());
    for (const auto &mutation : batch.mutations) {
      auto bytes = encode_mutation(mutation);
      if (bytes.size() > kMaxBatchBytes - staged_bytes)
        return {Status::Error("batch exceeds 16 MiB", ErrorCode::limitExceeded), {}};
      staged_bytes += bytes.size();
      payloads.push_back(std::move(bytes));
    }
  } catch (const std::length_error &e) {
    return {Status::Error(e.what(), ErrorCode::limitExceeded), {}};
  } catch (const std::invalid_argument &e) {
    return {Status::Error(e.what(), ErrorCode::invalidArgument), {}};
  }
  std::lock_guard lock(mu_);
  if (!opened_ || failed_ || !validated_ || repair_needed_)
    return {Status::Error("WAL requires validated recovery", ErrorCode::closed), {}};
  if (transaction_ids_.count(batch.transaction_id))
    return {Status::Error("transaction identity already used", ErrorCode::conflict), {}};
  if (next_lsn_ > UINT64_MAX - batch.mutations.size() - 2)
    return {Status::Error("LSN exhausted", ErrorCode::limitExceeded), {}};
  uint64_t sequence = next_lsn_;
  std::vector<uint8_t> begin;
  encode_u32(begin, uint32_t(payloads.size()));
  encode_u64(begin, staged_bytes);
  auto bytes = frame(sequence++, 1, id, begin);
  for (const auto &payload : payloads) {
    auto part = frame(sequence++, 2, id, payload);
    bytes.insert(bytes.end(), part.begin(), part.end());
  }
  auto digest = crc32_compute(bytes.data(), bytes.size());
  std::vector<uint8_t> commit;
  encode_u32(commit, uint32_t(payloads.size()));
  encode_u32(commit, digest);
  auto end = frame(sequence++, 3, id, commit);
  if (bytes.size() + end.size() > max_wal_bytes - byte_count_)
    return {Status::Error("WAL capacity reached; checkpoint required", ErrorCode::limitExceeded),
            {}};
  CommitReceipt receipt{batch.transaction_id, sequence - 1};
  transaction_ids_.insert(batch.transaction_id);
  auto exception_failure =
      Status::Error("commit IO interrupted; reopen required", ErrorCode::commitOutcomeUnknown,
                    {path_.string(), 0, {}, batch.transaction_id});
  Status status;
  try {
    status = io_->write_all(path_, bytes, WriteMode::append);
    if (status.ok) {
      io_->fault_point("wal.after_bytes");
      status = io_->write_all(path_, end, WriteMode::append);
    }
    if (status.ok) {
      io_->fault_point("wal.after_commit");
      status = io_->sync_file(path_);
    }
    if (status.ok)
      io_->fault_point("wal.after_sync");
  } catch (const FaultInterruption &) {
    failed_ = true;
    throw;
  } catch (...) {
    failed_ = true;
    return {std::move(exception_failure), {}};
  }
  if (!status.ok) {
    failed_ = true;
    status.code = ErrorCode::commitOutcomeUnknown;
    status.context.transaction_id = batch.transaction_id;
    return {std::move(status), {}};
  }
  byte_count_ += bytes.size() + end.size();
  valid_bytes_ = byte_count_;
  next_lsn_ = sequence;
  committed_lsn_ = receipt.committed_lsn;
  return {Status::OK(), std::move(receipt)};
}
Result<std::vector<RecoveredTransaction>> WalManager::recover_transactions() {
  std::lock_guard lock(mu_);
  if (!opened_ || failed_)
    return {Status::Error("WAL closed/fenced", ErrorCode::closed), {}};
  validated_ = false;
  auto data = io_->read(path_, max_wal_bytes);
  if (!data) {
    failed_ = true;
    return {data.status, {}};
  }
  auto corrupt = [&](const char *message) -> Result<std::vector<RecoveredTransaction>> {
    failed_ = true;
    return {Status::Error(message, ErrorCode::corruptData, {path_.string()}), {}};
  };
  auto header = make_header();
  if (data.value.size() < header.size() ||
      !std::equal(header.begin(), header.end(), data.value.begin()))
    return corrupt("WAL header changed during open");
  ByteReader r(data.value.data(), data.value.size());
  r.skip(header_bytes);
  uint64_t sequence = first_lsn_;
  size_t valid_end = header_bytes;
  uint64_t committed = first_lsn_ - 1;
  std::vector<RecoveredTransaction> groups;
  TransactionBatch pending;
  uint32_t count = 0, digest = 0;
  uint64_t declared_bytes = 0, actual_bytes = 0;
  bool active = false;
  std::unordered_set<std::string> ids;
  while (r.remaining()) {
    size_t start = r.position();
    uint32_t length;
    if (!r.read_u32(length))
      break;
    if (length < 25 || length > max_wal_bytes)
      return corrupt("invalid WAL frame length");
    if (!r.has_bytes(size_t(length) + 4))
      break;
    ByteReader body(data.value.data() + r.position(), length);
    uint64_t lsn;
    uint8_t kind;
    UUID id;
    body.read_u64(lsn);
    body.read_u8(kind);
    read_uuid(body, id);
    r.skip(length);
    uint32_t crc;
    r.read_u32(crc);
    if (crc != crc32_compute(data.value.data() + start, 4 + length))
      return corrupt("WAL frame checksum mismatch");
    if (lsn != sequence || sequence == UINT64_MAX)
      return corrupt("nonconsecutive WAL LSN");
    ++sequence;
    auto name = uuid_string(id);
    if (kind == 1) {
      if (active)
        return corrupt("nested WAL transaction");
      if (!body.read_u32(count) || !body.read_u64(declared_bytes) || body.remaining() || !count ||
          count > kMaxBatchStatements || declared_bytes > kMaxBatchBytes)
        return corrupt("invalid WAL begin counts");
      if (!ids.insert(name).second)
        return corrupt("duplicate WAL transaction identity");
      pending = {std::move(name), {}};
      pending.mutations.reserve(count);
      actual_bytes = 0;
      digest = 0;
      active = true;
    } else if (kind == 2) {
      if (!active || pending.transaction_id != name || pending.mutations.size() >= count)
        return corrupt("unexpected WAL mutation");
      size_t size = body.remaining();
      if (size > declared_bytes - actual_bytes)
        return corrupt("WAL staged byte mismatch");
      actual_bytes += size;
      auto mutation = decode_mutation(data.value.data() + start + 4 + body.position(), size);
      if (!mutation)
        return corrupt("invalid WAL mutation");
      pending.mutations.push_back(std::move(mutation.value));
    } else if (kind == 3) {
      uint32_t final_count, final_digest;
      if (!active || pending.transaction_id != name || !body.read_u32(final_count) ||
          !body.read_u32(final_digest) || body.remaining() || final_count != count ||
          pending.mutations.size() != count || actual_bytes != declared_bytes ||
          digest != final_digest)
        return corrupt("invalid WAL commit group");
      groups.push_back({std::move(pending), lsn});
      committed = lsn;
      valid_end = r.position();
      active = false;
    } else
      return corrupt("invalid WAL frame kind");
    if (kind != 3)
      digest = crc32_compute(data.value.data() + start, r.position() - start, digest);
  }
  byte_count_ = data.value.size();
  valid_bytes_ = valid_end;
  repair_needed_ = valid_end != byte_count_;
  next_lsn_ = committed + 1;
  committed_lsn_ = committed;
  transaction_ids_.clear();
  for (const auto &g : groups)
    transaction_ids_.insert(g.batch.transaction_id);
  validated_ = true;
  return {Status::OK(), std::move(groups)};
}
Status WalManager::repair_tail() {
  std::lock_guard lock(mu_);
  if (!opened_ || failed_ || !validated_)
    return Status::Error("WAL repair requires validated history", ErrorCode::closed);
  if (!repair_needed_)
    return Status::OK();
  auto status = io_->truncate(path_, valid_bytes_);
  if (status.ok)
    status = io_->sync_file(path_);
  if (!status.ok) {
    failed_ = true;
    return status;
  }
  byte_count_ = valid_bytes_;
  repair_needed_ = false;
  return Status::OK();
}
Result<uint64_t> WalManager::append_single(Mutation mutation) {
  auto result = append_transaction({new_transaction_id(), {std::move(mutation)}});
  if (!result)
    return {result.status, 0};
  return {Status::OK(), result.value.committed_lsn};
}
Result<uint64_t> WalManager::append_upsert_node(const std::string &label, const std::string &id,
                                                const PropertyMap &props) {
  return append_single(UpsertNode{label, id, props});
}
Result<uint64_t> WalManager::append_upsert_edge(const std::string &type, const std::string &from,
                                                const std::string &to, const PropertyMap &props) {
  return append_single(UpsertEdge{type, from, to, props});
}
Result<uint64_t> WalManager::append_delete_node(const std::string &id) {
  return append_single(DeleteNode{id});
}
Result<uint64_t> WalManager::append_delete_edge(const std::string &from, const std::string &type,
                                                const std::string &to) {
  return append_single(DeleteEdge{from, type, to});
}
Result<uint64_t> WalManager::append_create_index(const std::string &label,
                                                 const std::string &prop) {
  return append_single(CreateIndex{label, prop});
}
Result<std::vector<WalEntry>> WalManager::recover_entries() {
  auto groups = recover_transactions();
  if (!groups)
    return {groups.status, {}};
  std::vector<WalEntry> entries;
  for (auto &group : groups.value)
    for (auto &mutation : group.batch.mutations) {
      WalEntry e;
      e.lsn = group.committed_lsn;
      e.op = static_cast<WalOpType>(mutation.index() + 1);
      std::visit(
          [&](auto &m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, UpsertNode>) {
              e.label_or_type = std::move(m.label);
              e.id = std::move(m.id);
              e.properties = std::move(m.properties);
              e.modified_ms = m.modified_ms;
            }
            if constexpr (std::is_same_v<T, UpsertEdge>) {
              e.label_or_type = std::move(m.type);
              e.from = std::move(m.from);
              e.to = std::move(m.to);
              e.properties = std::move(m.properties);
              e.modified_ms = m.modified_ms;
            }
            if constexpr (std::is_same_v<T, DeleteNode>)
              e.id = std::move(m.id);
            if constexpr (std::is_same_v<T, DeleteEdge>) {
              e.from = std::move(m.from);
              e.to = std::move(m.to);
              e.label_or_type = std::move(m.type);
            }
            if constexpr (std::is_same_v<T, CreateIndex>) {
              e.label_or_type = std::move(m.label);
              e.index_property = std::move(m.property);
            }
          },
          mutation);
      entries.push_back(std::move(e));
    }
  return {Status::OK(), std::move(entries)};
}
Status WalManager::truncate_after_checkpoint(uint64_t checkpoint_lsn) {
  std::lock_guard lock(mu_);
  if (!opened_ || failed_ || !validated_ || repair_needed_)
    return Status::Error("WAL closed/fenced", ErrorCode::closed);
  if (checkpoint_lsn != committed_lsn_ || checkpoint_lsn == UINT64_MAX)
    return Status::Error("checkpoint does not cover WAL", ErrorCode::conflict);
  first_lsn_ = checkpoint_lsn + 1;
  predecessor_id_ = segment_id_++;
  auto status = atomic_write(*io_, path_, make_header());
  if (!status.ok) {
    failed_ = true;
    return status;
  }
  next_lsn_ = first_lsn_;
  byte_count_ = valid_bytes_ = header_bytes;
  transaction_ids_.clear();
  return Status::OK();
}
} // namespace graphdb
