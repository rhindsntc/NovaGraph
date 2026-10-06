#pragma once
#include "graphdb/Status.hpp"
#include <filesystem>
#include <memory>
namespace graphdb {
class DatabaseLock {
  int fd_;
  std::filesystem::path path_;
  DatabaseLock(int fd, std::filesystem::path path) : fd_(fd), path_(std::move(path)) {}

public:
  ~DatabaseLock();
  DatabaseLock(const DatabaseLock &) = delete;
  DatabaseLock &operator=(const DatabaseLock &) = delete;
  static Result<std::unique_ptr<DatabaseLock>> acquire(const std::filesystem::path &directory,
                                                       bool create = true);
  const std::filesystem::path &directory() const { return path_; }
};
} // namespace graphdb
