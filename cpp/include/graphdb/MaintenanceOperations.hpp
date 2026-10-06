#pragma once
#include "graphdb/Catalog.hpp"
#include "graphdb/Transaction.hpp"
namespace graphdb {
class GraphEngine;
struct MaintenanceReport {
  std::string format;
  uint64_t generation{0}, committed_lsn{0};
  size_t nodes{0}, edges{0}, indexes{0};
  bool tail_repair_needed{false};
  std::vector<std::string> warnings;
};
// Offline operations never create or repair a source implicitly. All callers must
// stop legacy writers; their format predates Nova's ownership protocol.
Result<MaintenanceReport> maintain(const std::string &command, const std::filesystem::path &source,
                                   const std::filesystem::path &destination = {},
                                   std::shared_ptr<FileIO> io = default_file_io());
std::string maintenance_json(const MaintenanceReport &);
class MaintenanceAccess {
public:
  static Result<CatalogData> snapshot(GraphEngine &);
  static Result<CatalogData> replay(CatalogData, const std::vector<RecoveredTransaction> &,
                                    bool legacy = false);
};
Result<MaintenanceReport> publish_backup(CatalogData, const std::filesystem::path &source,
                                         const std::filesystem::path &destination,
                                         std::shared_ptr<FileIO>);
Result<CatalogData> read_legacy_v1(const std::filesystem::path &, std::shared_ptr<FileIO>);
} // namespace graphdb
