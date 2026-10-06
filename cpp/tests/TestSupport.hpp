#pragma once
#include "graphdb/CGraphDB.h"
#include "graphdb/GraphEngine.hpp"
#include <condition_variable>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
namespace nova_test {
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
struct KnownFailure : Failure {
  std::string id;
  KnownFailure(std::string key, std::string message) : Failure(message), id(std::move(key)) {}
};
inline void check(bool value, const char* expr, const char* file, int line) {
  if (!value) throw Failure(std::string(file)+":"+std::to_string(line)+": "+expr);
}
inline void contract(bool value, const char* id, const char* expr) {
  if (!value) throw KnownFailure(id, expr);
}
struct Case { std::string id, suite, owner; std::function<void()> run; };
inline std::vector<Case>& cases() { static std::vector<Case> all; return all; }
struct Register {
  Register(const char* id, const char* suite, const char* owner, std::function<void()> run) {
    cases().push_back({id,suite,owner,std::move(run)});
  }
};
class TempDirectory {
  std::filesystem::path path_;
public:
  TempDirectory() {
    auto pattern=(std::filesystem::temp_directory_path()/"nova-test-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw Failure("mkdtemp failed");
    path_=pattern;
  }
  ~TempDirectory() { std::error_code ec; std::filesystem::remove_all(path_,ec); }
  const std::filesystem::path& path() const { return path_; }
};
inline std::string take(GraphDBString value) {
  struct Guard { GraphDBString value; ~Guard() { graphdb_string_free(value); } } guard{value};
  if (!value.data) throw Failure("null C response");
  return std::string(value.data,value.len);
}
struct Handle {
  GraphDBHandle* value;
  // Bound the unrelated worker-shutdown wait while testing C API semantics.
  // The engine can miss a shutdown notification; T15 owns lifecycle repair.
  explicit Handle(const std::filesystem::path& path) : value(graphdb_open_with_config(path.c_str(),600000,10)) {
    if (!value) throw Failure("could not open fixture database");
  }
  ~Handle() { graphdb_close(value); }
  Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
  std::string query(const char* text,const char* params=nullptr) {
    return take(graphdb_execute_query_with_params(value,text,params));
  }
};
struct EngineFixture {
  TempDirectory dir;
  std::shared_ptr<graphdb::MemoryHotStore> hot=std::make_shared<graphdb::MemoryHotStore>();
  std::shared_ptr<graphdb::FileDiskStore> disk=std::make_shared<graphdb::FileDiskStore>(dir.path()/"cold");
  graphdb::EngineConfig config;
  std::unique_ptr<graphdb::GraphEngine> engine;
  EngineFixture() { config.database_dir=dir.path(); engine=std::make_unique<graphdb::GraphEngine>(hot,disk,config); }
  // Deterministic cold transition: no sleeps or background worker.
  void cold(const std::string& key) {
    auto rec=hot->get(key); if (!rec) throw Failure("fixture record missing");
    auto written=disk->write(key,rec->object); if (!written) throw Failure(written.status.message);
    auto descriptor=graphdb::describe_payload(*disk,written.value);if(!descriptor)throw Failure(descriptor.status.message);
    if (!hot->move_to_cold_if_version(key,rec->object.version,written.value,descriptor.value.bytes,descriptor.value.crc,rec->activity_version)) throw Failure("fixture version changed");
  }
};
// Pass time explicitly through the existing HotStore interface.
struct ManualClock { int64_t now{100}; void advance(int64_t ms) { now+=ms; } };
class OperationBarrier {
  std::mutex mutex_; std::condition_variable cv_; bool reached_{false},released_{false};
public:
  void arrive_and_wait() {
    std::unique_lock lock(mutex_); reached_=true; cv_.notify_all();
    if (!cv_.wait_for(lock,std::chrono::seconds(5),[&]{return released_;})) throw Failure("barrier release timeout");
  }
  void wait_until_reached() {
    std::unique_lock lock(mutex_);
    if (!cv_.wait_for(lock,std::chrono::seconds(5),[&]{return reached_;})) throw Failure("barrier arrival timeout");
  }
  void release() { std::lock_guard lock(mutex_); released_=true; cv_.notify_all(); }
};
inline std::filesystem::path current_catalog(const std::filesystem::path& dir) {auto root=graphdb::Recovery::read_root(dir);if(!root)throw Failure(root.status.message);return dir/root.value.current.filename;}
inline std::filesystem::path active_wal(const std::filesystem::path& dir) {auto root=graphdb::Recovery::read_root(dir);if(!root)throw Failure(root.status.message);return dir/root.value.active.filename;}
// Existing DiskStore is the injection seam; faults remain test-only.
class FailingDiskStore : public graphdb::DiskStore {
public:
  graphdb::Result<std::string> write(const std::string&,const graphdb::GraphObject&) override { return {graphdb::Status::Error("injected write failure"),{}}; }
  graphdb::Result<graphdb::GraphObject> read(const std::string&) override { return {graphdb::Status::Error("injected read failure"),{}}; }
  graphdb::Status remove(const std::string&) override { return graphdb::Status::Error("injected remove failure"); }
};
}
#define CHECK(expr) ::nova_test::check(static_cast<bool>(expr),#expr,__FILE__,__LINE__)
#define CONTRACT(id,expr) ::nova_test::contract(static_cast<bool>(expr),id,#expr)
#define NOVA_TEST(name,suite,owner) \
  static void name(); \
  static ::nova_test::Register register_##name(#name,suite,owner,name); \
  static void name()
