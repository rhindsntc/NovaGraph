#pragma once
#include "../ModelFileIO.hpp"
#include <iostream>
#include <set>
#include <signal.h>
namespace nova_test {
struct PowerLoss : graphdb::FaultInterruption {
  const char *what() const noexcept override { return "modeled power loss"; }
};
template <class Base> class FaultIO : public Base {
  std::string category(const std::filesystem::path &path) const {
    auto n = path.filename().string();
    if (n.starts_with("CURRENT"))
      return "root";
    if (n.starts_with("catalog-"))
      return "catalog";
    if (n.starts_with("payload-"))
      return "payloadFile";
    if (n.starts_with("wal-"))
      return "walFile";
    return "directory";
  }
  template <class F> auto around(const std::filesystem::path &path, const char *action, F f) {
    auto name = category(path) + "." + action;
    fault_point(name + ".before");
    auto result = f();
    fault_point(name + ".after");
    return result;
  }

public:
  bool armed = false, signal_crash = false, print_events = false;
  size_t counter = 0, crash_at = 0;
  std::string last_event;
  std::set<std::string> events;
  void fault_point(std::string_view point) override {
    if (!armed)
      return;
    last_event = point;
    events.insert(last_event);
    ++counter;
    if (print_events)
      std::cout << "EVENT " << counter << " " << point << std::endl;
    if (crash_at && counter == crash_at) {
      armed = false;
      if (signal_crash) {
        ::kill(::getpid(), SIGKILL);
        ::_exit(99);
      }
      throw PowerLoss();
    }
  }
  graphdb::Result<std::vector<uint8_t>> read(const std::filesystem::path &path,
                                             size_t maximum) override {
    return around(path, "read", [&] { return Base::read(path, maximum); });
  }
  graphdb::Result<std::unique_ptr<graphdb::SequentialReader>> open_reader(
      const std::filesystem::path &path,size_t maximum,size_t fallback) override {
    auto result=around(path,"openReader",[&]{return Base::open_reader(path,maximum,fallback);});
    if(!result)return {result.status,{}};
    struct Reader:graphdb::SequentialReader {
      FaultIO *owner;std::filesystem::path path;std::unique_ptr<graphdb::SequentialReader> inner;
      Reader(FaultIO *o,std::filesystem::path p,std::unique_ptr<graphdb::SequentialReader> r):owner(o),path(std::move(p)),inner(std::move(r)){}
      uint64_t size() const override{return inner->size();}
      graphdb::Result<size_t> read(uint8_t *out,size_t n) override {
        return owner->around(path,"streamRead",[&]{return inner->read(out,n);});
      }
      graphdb::Status close() override{return owner->around(path,"closeReader",[&]{return inner->close();});}
    };
    return {graphdb::Status::OK(),std::make_unique<Reader>(this,path,std::move(result.value))};
  }
  graphdb::Result<std::vector<std::string>> list(const std::filesystem::path &path,
                                                 size_t offset = 0, size_t limit = 4096) override {
    return around(path, "list", [&] { return Base::list(path, offset, limit); });
  }
  graphdb::Status write_all(const std::filesystem::path &path, const std::vector<uint8_t> &bytes,
                            graphdb::WriteMode mode) override {
    return around(path, "write", [&] { return Base::write_all(path, bytes, mode); });
  }
  graphdb::Status sync_file(const std::filesystem::path &path) override {
    return around(path, "sync", [&] { return Base::sync_file(path); });
  }
  graphdb::Status sync_directory(const std::filesystem::path &path) override {
    return around(path, "syncDirectory", [&] { return Base::sync_directory(path); });
  }
  graphdb::Status rename(const std::filesystem::path &from,
                         const std::filesystem::path &to) override {
    return around(to, "rename", [&] { return Base::rename(from, to); });
  }
  graphdb::Status truncate(const std::filesystem::path &path, uint64_t bytes) override {
    return around(path, "truncate", [&] { return Base::truncate(path, bytes); });
  }
  graphdb::Status remove(const std::filesystem::path &path) override {
    return around(path, "remove", [&] { return Base::remove(path); });
  }
};
} // namespace nova_test
