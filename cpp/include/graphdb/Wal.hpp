#pragma once

#include "graphdb/BinaryEncoding.hpp"
#include "graphdb/FileIO.hpp"
#include "graphdb/GraphTypes.hpp"
#include "graphdb/Transaction.hpp"
#include <atomic>
#include <unordered_set>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace graphdb {

enum class WalOpType : uint8_t {
  UpsertNode = 1,
  UpsertEdge = 2,
  DeleteNode = 3,
  DeleteEdge = 4,
  CreateIndex = 5,
  Checkpoint = 6
};

struct WalEntry {
  uint64_t lsn{0};
  std::optional<int64_t> modified_ms;
  WalOpType op{WalOpType::UpsertNode};
  std::string id;
  std::string label_or_type;
  std::string from;
  std::string to;
  PropertyMap properties;
  std::string index_property;
};

class WalManager {
public:
  explicit WalManager(std::filesystem::path wal_path,
                      std::shared_ptr<FileIO> io = default_file_io());
  ~WalManager();

  WalManager(const WalManager &) = delete;
  WalManager &operator=(const WalManager &) = delete;

  Status open();
  Status create(const UUID &, uint64_t segment, uint64_t predecessor, uint64_t first);
  uint64_t segment_id() const { return segment_id_; }
  uint64_t predecessor_id() const { return predecessor_id_; }
  bool needs_repair() const { return repair_needed_; }
  Result<CommitReceipt> append_transaction(const TransactionBatch &batch);
  Result<std::vector<RecoveredTransaction>> recover_transactions();
  Status repair_tail();
  uint64_t first_lsn() const { return first_lsn_; }
  uint64_t committed_lsn() const { return committed_lsn_; }
  const UUID &database_id() const { return database_id_; }
  static constexpr size_t header_bytes = 52;
  static constexpr uint32_t format_version = 4;
  void close();

  // Appends mutation to WAL and flushes to disk.
  Result<uint64_t> append_upsert_node(const std::string &label, const std::string &id,
                                      const PropertyMap &properties);
  Result<uint64_t> append_upsert_edge(const std::string &type, const std::string &from,
                                      const std::string &to, const PropertyMap &properties);
  Result<uint64_t> append_delete_node(const std::string &id);
  Result<uint64_t> append_delete_edge(const std::string &from, const std::string &type,
                                      const std::string &to);
  Result<uint64_t> append_create_index(const std::string &label, const std::string &property);

  // Reads all valid entries from WAL.
  Result<std::vector<WalEntry>> recover_entries();

  // Truncates WAL to initial state starting after the given LSN.
  Status truncate_after_checkpoint(uint64_t checkpoint_lsn);

  bool writable() const {
    std::lock_guard lock(mu_);
    return opened_ && !failed_ && validated_ && !repair_needed_;
  }
  uint64_t next_lsn() const { return next_lsn_; }

private:
  std::vector<uint8_t> make_header() const;
  Result<uint64_t> append_single(Mutation mutation);

  std::filesystem::path path_;
  mutable std::mutex mu_;
  std::shared_ptr<FileIO> io_;
  bool opened_{false}, failed_{false};
  std::atomic<uint64_t> next_lsn_{1};
  size_t byte_count_{header_bytes}, valid_bytes_{header_bytes};
  bool validated_{false}, repair_needed_{false};
  UUID database_id_{};
  uint64_t segment_id_{1}, predecessor_id_{0}, first_lsn_{1};
  std::atomic<uint64_t> committed_lsn_{0};
  std::unordered_set<std::string> transaction_ids_;
};

} // namespace graphdb
