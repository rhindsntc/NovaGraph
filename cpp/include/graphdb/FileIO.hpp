#pragma once
#include "graphdb/Status.hpp"
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <string_view>
#include <unistd.h>
#include <vector>
namespace graphdb {
struct FaultInterruption : std::exception {
  const char *what() const noexcept override { return "fault campaign interruption"; }
};
enum class WriteMode { exclusive, replace, append };
class SequentialReader {
public:
  virtual ~SequentialReader()=default;
  virtual uint64_t size() const=0;
  virtual Result<size_t> read(uint8_t *,size_t)=0;
  virtual Status close()=0;
};
class FileIO {
public:
  virtual ~FileIO() = default;
  // Internal fault-campaign seam; production backends leave it inert.
  virtual void fault_point(std::string_view) {}
  virtual Result<std::vector<std::string>> list(const std::filesystem::path &, size_t offset = 0,
                                                size_t limit = 4096) = 0;
  virtual Result<std::vector<uint8_t>> read(const std::filesystem::path &, size_t maximum) = 0;
  // Caller reserves fallback_limit and reader overhead before opening.
  virtual Result<std::unique_ptr<SequentialReader>> open_reader(
      const std::filesystem::path &,size_t maximum,size_t fallback_limit);
  virtual Status write_all(const std::filesystem::path &, const std::vector<uint8_t> &,
                           WriteMode) = 0;
  virtual Status sync_file(const std::filesystem::path &) = 0;
  // Durably sync files on one device before their references are published.
  // Backends without a group primitive retain the per-file durability contract.
  virtual Status sync_files(const std::vector<std::filesystem::path> &paths) {
    for (const auto &path : paths) {
      auto status = sync_file(path);
      if (!status.ok)
        return status;
    }
    return Status::OK();
  }
  virtual Status sync_directory(const std::filesystem::path &) = 0;
  virtual Status rename(const std::filesystem::path &, const std::filesystem::path &) = 0;
  virtual Status truncate(const std::filesystem::path &, uint64_t bytes) = 0;
  virtual Status remove(const std::filesystem::path &) = 0;
};
class PosixFileIO : public FileIO {
protected:
  virtual ssize_t write_some(int fd, const void *data, size_t size) {
    return ::write(fd, data, size);
  }
  virtual ssize_t read_some(int fd, void *data, size_t size) { return ::read(fd, data, size); }
  virtual int sync_descriptor(int fd, bool full);

public:
  Result<std::vector<std::string>> list(const std::filesystem::path &, size_t offset = 0,
                                        size_t limit = 4096) override;
  Result<std::vector<uint8_t>> read(const std::filesystem::path &, size_t maximum) override;
  Result<std::unique_ptr<SequentialReader>> open_reader(
      const std::filesystem::path &,size_t maximum,size_t fallback_limit) override;
  Status write_all(const std::filesystem::path &, const std::vector<uint8_t> &, WriteMode) override;
  Status sync_file(const std::filesystem::path &) override;
  Status sync_files(const std::vector<std::filesystem::path> &) override;
  Status sync_directory(const std::filesystem::path &) override;
  Status rename(const std::filesystem::path &, const std::filesystem::path &) override;
  Status truncate(const std::filesystem::path &, uint64_t bytes) override;
  Status remove(const std::filesystem::path &) override;
};
std::shared_ptr<FileIO> default_file_io();
Status atomic_write(FileIO &, const std::filesystem::path &, const std::vector<uint8_t> &);
} // namespace graphdb
