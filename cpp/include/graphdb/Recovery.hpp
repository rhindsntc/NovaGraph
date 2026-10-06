#pragma once
#include "graphdb/Catalog.hpp"
#include "graphdb/Wal.hpp"
namespace graphdb {
struct CatalogDescriptor {
  std::string filename;
  uint32_t version{3};
  uint64_t bytes{0};
  uint32_t crc{0};
  uint64_t generation{0}, committed_lsn{0};
};
struct WalDescriptor {
  std::string filename;
  uint32_t version{WalManager::format_version};
  UUID database_id{};
  uint64_t bytes{0};
  uint32_t crc{0};
  uint64_t segment{0}, predecessor{0}, first_lsn{0}, last_lsn{0};
};
struct DatabaseRoot {
  UUID database_id{};
  uint64_t generation{1};
  CatalogDescriptor current;
  std::optional<CatalogDescriptor> previous;
  WalDescriptor active;
  std::vector<WalDescriptor> sealed;
};
class Recovery {
  std::filesystem::path directory_;
  std::shared_ptr<FileIO> io_;
  DatabaseRoot root_;
  CatalogData catalog_;
  std::shared_ptr<WalManager> wal_;
  std::vector<RecoveredTransaction> transactions_;
  bool fenced_{false};
  Recovery(std::filesystem::path path, std::shared_ptr<FileIO> io)
      : directory_(std::move(path)), io_(std::move(io)) {}

public:
  static Result<std::shared_ptr<Recovery>> open(const std::filesystem::path &,
                                                std::shared_ptr<FileIO> io = default_file_io(),
                                                bool create = true,
                                                CatalogReadMode mode = CatalogReadMode::strict);
  static Status install_snapshot(const std::filesystem::path &, CatalogData,
                                 std::shared_ptr<FileIO>);
  static Result<DatabaseRoot> read_root(const std::filesystem::path &,
                                        std::shared_ptr<FileIO> io = default_file_io());
  const DatabaseRoot &root() const { return root_; }
  const CatalogData &catalog() const { return catalog_; }
  const std::vector<RecoveredTransaction> &transactions() const { return transactions_; }
  std::shared_ptr<WalManager> wal() const { return wal_; }
  Status repair_tail() { return wal_->repair_tail(); }
  Status checkpoint(CatalogData data);
  bool fenced() const { return fenced_; }
};
} // namespace graphdb
