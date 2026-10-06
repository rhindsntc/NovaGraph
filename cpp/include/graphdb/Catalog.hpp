#pragma once

#include "graphdb/BinaryEncoding.hpp"
#include "graphdb/GraphTypes.hpp"
#include "graphdb/Storage.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace graphdb {

struct CatalogData {
  std::array<uint8_t, 16> database_id{};
  uint64_t generation{1};
  uint64_t last_lsn{0};
  IndexState indexes;
  std::vector<std::pair<std::string, std::string>> declared_indexes;
  std::vector<StoredRecord> records;
};

struct CatalogDescriptor;
inline constexpr size_t catalog_scan_workspace=2*16384+4096;
// Only for descriptors already validated by Recovery. Success authenticates all
// bytes against that descriptor; callbacks must only mark, never delete files.
Status scan_retained_catalog(FileIO &,const std::filesystem::path &,const CatalogDescriptor &,
    const std::array<uint8_t,16> &,QueryContext &,const std::function<void(std::string_view)> &);

enum class CatalogReadMode { strict, rebuild_derived };
class Catalog {
public:
  static Status save(const std::filesystem::path &, const CatalogData &,
                     std::shared_ptr<FileIO> io = default_file_io());
  static Status save(const std::filesystem::path &catalog_path, uint64_t last_lsn,
                     const std::vector<StoredRecord> &records,
                     const std::vector<std::pair<std::string, std::string>> &declared_indexes,
                     std::shared_ptr<FileIO> io = default_file_io());

  static Result<CatalogData> load(const std::filesystem::path &catalog_path,
                                  std::shared_ptr<FileIO> io = default_file_io(),
                                  CatalogReadMode mode = CatalogReadMode::strict);
};

} // namespace graphdb
