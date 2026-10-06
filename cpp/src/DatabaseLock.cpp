#include "graphdb/DatabaseLock.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
namespace graphdb {
DatabaseLock::~DatabaseLock() {
  if (fd_ >= 0)
    ::close(fd_);
}
Result<std::unique_ptr<DatabaseLock>> DatabaseLock::acquire(const std::filesystem::path &directory,
                                                            bool create) {
  std::error_code error;
  if (create)
    std::filesystem::create_directories(directory, error);
  if (error)
    return {
        Status::Error(error.message(), ErrorCode::ioFailure, {directory.string(), error.value()}),
        {}};
  auto root = std::filesystem::canonical(directory, error);
  if (error)
    return {
        Status::Error(error.message(), ErrorCode::ioFailure, {directory.string(), error.value()}),
        {}};
  auto path = root / ".nova.lock";
  bool directory_lock = !create && !std::filesystem::exists(path);
  if (directory_lock && std::filesystem::exists(root / "CURRENT"))
    return {Status::Error("current database ownership file is missing; restore the complete "
                          "directory before offline maintenance",
                          ErrorCode::corruptData, {path.string()}),
            {}};
  if (directory_lock)
    path = root;
  int fd;
  do {
    fd = ::open(path.c_str(),
                (directory_lock ? O_RDONLY | O_DIRECTORY : (create ? O_RDWR : O_RDONLY)) |
                    (create ? O_CREAT : 0) | O_CLOEXEC | O_NOFOLLOW,
                0600);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0)
    return {Status::Error("lock open failed", ErrorCode::ioFailure, {path.string(), errno}), {}};
  int result;
  do {
    result = flock(fd, LOCK_EX | LOCK_NB);
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    int code = errno;
    ::close(fd);
    return {Status::Error("database directory already owned or lock unavailable",
                          code == EWOULDBLOCK ? ErrorCode::busy : ErrorCode::ioFailure,
                          {path.string(), code}),
            {}};
  }
  try {
    return {Status::OK(), std::unique_ptr<DatabaseLock>(new DatabaseLock(fd, root))};
  } catch (...) {
    ::close(fd);
    throw;
  }
}
} // namespace graphdb
