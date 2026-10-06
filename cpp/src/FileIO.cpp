#include "graphdb/FileIO.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <functional>
#include <algorithm>
#include <sys/stat.h>
namespace graphdb {
Result<std::vector<std::string>> PosixFileIO::list(const std::filesystem::path &path, size_t offset,
                                                   size_t limit) {
  std::error_code ec;
  std::filesystem::directory_iterator it(path, ec);
  if (ec)
    return {Status::Error(ec.message(),
                          ec.value() == ENOENT ? ErrorCode::notFound : ErrorCode::ioFailure,
                          {path.string(), ec.value()}),
            {}};
  std::vector<std::string> names;
  size_t index = 0;
  for (auto end = std::filesystem::directory_iterator(); it != end && names.size() < limit;
       it.increment(ec)) {
    if (ec)
      return {Status::Error(ec.message(), ErrorCode::ioFailure), {}};
    if (index++ >= offset)
      names.push_back(it->path().filename().string());
  }
  if (ec)
    return {Status::Error(ec.message(), ErrorCode::ioFailure), {}};
  return {Status::OK(), std::move(names)};
}

namespace {
Status failure(const std::filesystem::path &path, const char *operation, int code = errno) {
  return Status::Error(std::string(operation) + ": " + strerror(code),
                       code == ENOENT ? ErrorCode::notFound : ErrorCode::ioFailure,
                       {path.string(), code, {}, {}});
}
struct FD {
  int value;
  ~FD() {
    if (value >= 0)
      ::close(value);
  }
};
int open_retry(const std::filesystem::path &path, int flags) {
  int fd;
  do {
    fd = ::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, 0600);
  } while (fd < 0 && errno == EINTR);
  return fd;
}
Status close_checked(FD &fd, const std::filesystem::path &path) {
  int raw = fd.value;
  fd.value = -1;
  if (::close(raw) < 0)
    return failure(path, "close");
  return Status::OK();
}
} // namespace
Result<std::vector<uint8_t>> PosixFileIO::read(const std::filesystem::path &path, size_t maximum) {
  FD fd{open_retry(path, O_RDONLY)};
  if (fd.value < 0)
    return {failure(path, "open read"), {}};
  struct stat info {};
  int stat_result;
  do {
    stat_result = fstat(fd.value, &info);
  } while (stat_result < 0 && errno == EINTR);
  if (stat_result < 0)
    return {failure(path, "stat"), {}};
  if (!S_ISREG(info.st_mode) || info.st_size < 0)
    return {Status::Error("expected regular file", ErrorCode::invalidArgument, {path.string()}),
            {}};
  if (uint64_t(info.st_size) > maximum)
    return {Status::Error("file exceeds read bound", ErrorCode::limitExceeded, {path.string()}),
            {}};
  std::vector<uint8_t> bytes(size_t(info.st_size));
  size_t offset = 0;
  while (offset < bytes.size()) {
    auto count = read_some(fd.value, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return {failure(path, "short/failed read", count == 0 ? EIO : errno), {}};
    offset += size_t(count);
  }
  auto status = close_checked(fd, path);
  if (!status.ok)
    return {status, {}};
  return {Status::OK(), std::move(bytes)};
}
Result<std::unique_ptr<SequentialReader>> FileIO::open_reader(
    const std::filesystem::path &path,size_t maximum,size_t fallback_limit) {
  auto bytes=read(path,std::min(maximum,fallback_limit));
  if(!bytes)return {bytes.status,{}};
  struct Reader:SequentialReader {
    std::vector<uint8_t> bytes;size_t offset=0;
    explicit Reader(std::vector<uint8_t> b):bytes(std::move(b)){}
    uint64_t size() const override{return bytes.size();}
    Result<size_t> read(uint8_t *out,size_t count) override {
      count=std::min(count,bytes.size()-offset);std::copy_n(bytes.data()+offset,count,out);offset+=count;
      return {Status::OK(),count};
    }
    Status close() override{return Status::OK();}
  };
  return {Status::OK(),std::make_unique<Reader>(std::move(bytes.value))};
}
Result<std::unique_ptr<SequentialReader>> PosixFileIO::open_reader(
    const std::filesystem::path &path,size_t maximum,size_t) {
  FD fd{open_retry(path,O_RDONLY)};
  if(fd.value<0)return {failure(path,"open reader"),{}};
  struct stat info{};int result;
  do{result=fstat(fd.value,&info);}while(result<0 && errno==EINTR);
  if(result<0)return {failure(path,"stat reader"),{}};
  if(!S_ISREG(info.st_mode) || info.st_size<0)return {Status::Error("expected regular file",ErrorCode::invalidArgument),{}};
  if(uint64_t(info.st_size)>maximum)return {Status::Error("file exceeds read bound",ErrorCode::limitExceeded),{}};
  struct Reader:SequentialReader {
    FD fd;uint64_t extent,offset=0;
    std::filesystem::path path;
    std::function<ssize_t(int,void*,size_t)> read_some;
    Reader(int raw,uint64_t n,std::filesystem::path p,std::function<ssize_t(int,void*,size_t)> f)
        :fd{raw},extent(n),path(std::move(p)),read_some(std::move(f)){}
    uint64_t size() const override{return extent;}
    Result<size_t> read(uint8_t *out,size_t count) override {
      if(fd.value<0)return {Status::Error("reader closed",ErrorCode::closed),0};
      count=std::min<uint64_t>(count,extent-offset);if(!count)return {Status::OK(),0};
      ssize_t n;do{n=read_some(fd.value,out,count);}while(n<0 && errno==EINTR);
      if(n<=0)return {failure(path,"short/failed stream read",n==0?EIO:errno),0};
      offset+=size_t(n);return {Status::OK(),size_t(n)};
    }
    Status close() override {
      if(fd.value<0)return Status::OK();
      // Detect an appended or truncated file even when its original bytes match.
      struct stat info{};int result;do{result=fstat(fd.value,&info);}while(result<0 && errno==EINTR);
      auto status=result<0?failure(path,"stat reader close"):
        (uint64_t(info.st_size)!=extent?Status::Error("stream extent changed",ErrorCode::corruptData):Status::OK());
      auto closed=close_checked(fd,path);return status.ok?closed:status;
    }
  };
  auto reader=std::make_unique<Reader>(fd.value,uint64_t(info.st_size),path,
      [this](int raw,void *out,size_t count){return read_some(raw,out,count);});
  fd.value=-1;return {Status::OK(),std::move(reader)};
}
Status PosixFileIO::write_all(const std::filesystem::path &path, const std::vector<uint8_t> &bytes,
                              WriteMode mode) {
  int flags = O_WRONLY | O_CREAT;
  if (mode == WriteMode::exclusive)
    flags |= O_EXCL;
  else if (mode == WriteMode::replace)
    flags |= O_TRUNC;
  else
    flags |= O_APPEND;
  FD fd{open_retry(path, flags)};
  if (fd.value < 0)
    return failure(path, "open write");
  size_t offset = 0;
  while (offset < bytes.size()) {
    auto count = write_some(fd.value, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return failure(path, "write", count == 0 ? EIO : errno);
    offset += size_t(count);
  }
  return close_checked(fd, path);
}
int PosixFileIO::sync_descriptor(int fd, bool full) {
#ifdef __APPLE__
  return full ? fcntl(fd, F_FULLFSYNC) : fsync(fd);
#else
  (void)full;
  return fsync(fd);
#endif
}
Status PosixFileIO::sync_file(const std::filesystem::path &path) {
  FD fd{open_retry(path, O_RDWR)};
  if (fd.value < 0)
    return failure(path, "open sync");
  int result;
  do {
    result = sync_descriptor(fd.value, true);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return failure(path, "file sync");
  return close_checked(fd, path);
}
Status PosixFileIO::sync_files(const std::vector<std::filesystem::path> &paths) {
  if (paths.empty())
    return Status::OK();
  FD anchor{open_retry(paths.front(), O_RDWR)};
  if (anchor.value < 0)
    return failure(paths.front(), "open group sync");
  struct stat anchor_info {};
  int result;
  do {
    result = fstat(anchor.value, &anchor_info);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return failure(paths.front(), "stat group sync");
  for (const auto &path : paths) {
    FD fd{open_retry(path, O_RDWR)};
    if (fd.value < 0)
      return failure(path, "open group member");
    struct stat info {};
    do {
      result = fstat(fd.value, &info);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
      return failure(path, "stat group member");
    if (info.st_dev != anchor_info.st_dev)
      return Status::Error("group sync requires one storage device", ErrorCode::invalidArgument);
    do {
      result = sync_descriptor(fd.value, false);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
      return failure(path, "group member sync");
    auto closed = close_checked(fd, path);
    if (!closed.ok)
      return closed;
    fault_point("payload.group.after_file_sync");
  }
#ifdef __APPLE__
  // fcntl(2): F_FULLFSYNC persists all data previously fsync'd on this device.
  // Keep the anchor open until this shared drive flush has succeeded.
  do {
    result = sync_descriptor(anchor.value, true);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return failure(paths.front(), "group full sync");
#endif
  return close_checked(anchor, paths.front());
}
Status PosixFileIO::sync_directory(const std::filesystem::path &path) {
  FD fd{open_retry(path, O_RDONLY | O_DIRECTORY)};
  if (fd.value < 0)
    return failure(path, "open directory");
  int result;
  do {
    result = fsync(fd.value);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return failure(path, "directory sync");
  return close_checked(fd, path);
}
Status PosixFileIO::rename(const std::filesystem::path &from, const std::filesystem::path &to) {
  int result;
  do {
    result = ::rename(from.c_str(), to.c_str());
  } while (result < 0 && errno == EINTR);
  return result < 0 ? failure(to, "rename") : Status::OK();
}
Status PosixFileIO::truncate(const std::filesystem::path &path, uint64_t bytes) {
  if (bytes > uint64_t(std::numeric_limits<off_t>::max()))
    return Status::Error("truncate bound", ErrorCode::limitExceeded);
  FD fd{open_retry(path, O_WRONLY)};
  if (fd.value < 0)
    return failure(path, "open truncate");
  int result;
  do {
    result = ftruncate(fd.value, off_t(bytes));
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return failure(path, "truncate");
  return close_checked(fd, path);
}
Status PosixFileIO::remove(const std::filesystem::path &path) {
  int result;
  do {
    result = unlink(path.c_str());
  } while (result < 0 && errno == EINTR);
  return result < 0 ? failure(path, "remove") : Status::OK();
}
std::shared_ptr<FileIO> default_file_io() {
  static auto io = std::make_shared<PosixFileIO>();
  return io;
}
Status atomic_write(FileIO &io, const std::filesystem::path &path,
                    const std::vector<uint8_t> &bytes) {
  bool root = path.filename() == "CURRENT";
  bool catalog = path.filename().string().starts_with("catalog-");
  auto temp = std::filesystem::path(path.string() + ".tmp");
  auto result = io.write_all(temp, bytes, WriteMode::replace);
  if (!result.ok)
    return result;
  if (root)
    io.fault_point("root.after_write");
  else if (catalog)
    io.fault_point("catalog.after_write");
  result = io.sync_file(temp);
  if (!result.ok)
    return result;
  if (root)
    io.fault_point("root.after_sync");
  else if (catalog)
    io.fault_point("catalog.after_sync");
  result = io.rename(temp, path);
  if (!result.ok)
    return result;
  if (root)
    io.fault_point("root.after_rename");
  else if (catalog)
    io.fault_point("catalog.after_rename");
  result = io.sync_directory(path.parent_path());
  if (result.ok) {
    if (root)
      io.fault_point("root.after_publish");
    else if (catalog)
      io.fault_point("catalog.after_publish");
  }
  return result;
}
} // namespace graphdb
