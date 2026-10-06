#include "TestSupport.hpp"
#include "graphdb/Catalog.hpp"
#include <fstream>
using namespace graphdb;
using namespace nova_test;
NOVA_TEST(duplicate_directory_owner_is_rejected, "io", "") {
  TempDirectory dir;
  Handle first(dir.path());
  auto second = graphdb_open(dir.path().c_str());
  bool rejected = !second;
  graphdb_close(second);
  CHECK(rejected);
}
NOVA_TEST(corrupt_open_preserves_original_catalog, "io", "") {
  TempDirectory dir;
  {
    Handle db(dir.path());
    CHECK(db.query("upsert node N a").find("\"ok\":true") != std::string::npos);
  }
  {
    std::ofstream out(current_catalog(dir.path()), std::ios::binary | std::ios::trunc);
    out << "corrupt bytes";
  }
  auto opened = graphdb_open(dir.path().c_str());
  bool rejected = !opened;
  graphdb_close(opened);
  CHECK(rejected);
  std::ifstream in(current_catalog(dir.path()), std::ios::binary);
  std::string bytes{std::istreambuf_iterator<char>(in), {}};
  CHECK(bytes == "corrupt bytes");
}
NOVA_TEST(corrupt_wal_open_preserves_original_bytes, "io", "") {
  TempDirectory dir;
  { Handle db(dir.path()); }
  {
    std::ofstream out(active_wal(dir.path()), std::ios::binary | std::ios::trunc);
    out << "bad";
  }
  auto opened = graphdb_open(dir.path().c_str());
  bool rejected = !opened;
  graphdb_close(opened);
  CHECK(rejected);
  std::ifstream in(active_wal(dir.path()), std::ios::binary);
  std::string bytes{std::istreambuf_iterator<char>(in), {}};
  CHECK(bytes == "bad");
}
NOVA_TEST(binary_corruption_never_uses_text_fallback, "io", "") {
  TempDirectory dir;
  FileDiskStore disk(dir.path());
  GraphObject object;
  object.id = "a";
  auto path = dir.path() / "text.rec";
  {
    std::ofstream out(path);
    out << serialize_object(object);
  }
  CHECK(!disk.read("text.rec"));
}
#include "graphdb/DatabaseLock.hpp"
#include "graphdb/FileIO.hpp"
#include "graphdb/QueryContext.hpp"
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
class ObservedSyncIO : public PosixFileIO {
public:
  size_t host_syncs{0}, full_syncs{0};
  bool interrupt_once{false}, fail_host{false}, fail_full{false};
protected:
  int sync_descriptor(int fd, bool full) override {
    if (interrupt_once) {
      interrupt_once = false;
      errno = EINTR;
      return -1;
    }
    full ? ++full_syncs : ++host_syncs;
    if ((full && fail_full) || (!full && fail_host)) {
      errno = EIO;
      return -1;
    }
    return PosixFileIO::sync_descriptor(fd, full);
  }
};
NOVA_TEST(posix_group_sync_preserves_durability_and_propagates_errors, "io", "") {
  TempDirectory dir;
  ObservedSyncIO io;
  std::vector<std::filesystem::path> paths = {dir.path()/"a", dir.path()/"b", dir.path()/"c"};
  for (const auto &path : paths)
    CHECK(io.write_all(path, {1, 2, 3}, WriteMode::exclusive).ok);
  io.interrupt_once = true;
  CHECK(io.sync_files(paths).ok);
  CHECK(io.host_syncs == 3);
#ifdef __APPLE__
  CHECK(io.full_syncs == 1);
  io.fail_full = true;
  CHECK(io.sync_files(paths).code == ErrorCode::ioFailure);
  io.fail_full = false;
#else
  CHECK(io.full_syncs == 0);
#endif
  auto full = io.full_syncs;
  io.fail_host = true;
  CHECK(io.sync_files(paths).code == ErrorCode::ioFailure);
  CHECK(io.full_syncs == full);
  io.fail_host = false;
  CHECK(io.sync_files({dir.path()/"missing"}).code == ErrorCode::notFound);
  CHECK(io.sync_files({}).ok);
  for (const auto &path : paths)
    CHECK(io.read(path, 3).value == std::vector<uint8_t>({1, 2, 3}));
}

NOVA_TEST(trim_full_flush_failure_never_publishes_cold_records, "io", "") {
  TempDirectory dir;
  auto io = std::make_shared<ObservedSyncIO>();
  auto hot = std::make_shared<MemoryHotStore>();
  auto disk = std::make_shared<FileDiskStore>(dir.path()/"cold", io);
  GraphObject object;
  object.id = "a";
  object.properties = {{"v", int64_t(42)}};
  CHECK(hot->put_hot("n/a", object).ok);
  EngineConfig config;
  config.database_dir = dir.path();
  config.file_io = io;
  config.checkpoint_on_destroy = false;
  GraphEngine engine(hot, disk, config);
  QueryContext context;
#ifdef __APPLE__
  io->fail_full = true;
#else
  io->fail_host = true;
#endif
  auto failed = engine.trim_memory(0, context);
  CHECK(!failed && failed.status.code == ErrorCode::ioFailure);
  CHECK(hot->get("n/a")->tier == StorageTier::Hot);
  CHECK(std::get<int64_t>(hot->get("n/a")->object.properties.at("v")) == 42);
}

NOVA_TEST(posix_io_round_trip_and_bounds, "io", "") {
  TempDirectory dir;
  PosixFileIO io;
  auto path = dir.path() / "data";
  std::vector<uint8_t> bytes = {0, 1, 2, 255};
  CHECK(io.write_all(path, bytes, WriteMode::exclusive).ok);
  CHECK(io.sync_file(path).ok);
  CHECK(io.sync_directory(dir.path()).ok);
  auto read = io.read(path, 4);
  CHECK(read);
  CHECK(read.value == bytes);
  CHECK(io.read(path, 3).status.code == ErrorCode::limitExceeded);
  CHECK(io.rename(path, dir.path() / "moved").ok);
  CHECK(io.truncate(dir.path() / "moved", 2).ok);
  CHECK(io.read(dir.path() / "moved", 4).value.size() == 2);
  CHECK(io.remove(dir.path() / "moved").ok);
  CHECK(io.read(dir.path() / "moved", 4).status.code == ErrorCode::notFound);
}
class InterruptedShortWriter : public PosixFileIO {
  bool interrupted = false;
  ssize_t write_some(int fd, const void *data, size_t size) override {
    if (!interrupted) {
      interrupted = true;
      errno = EINTR;
      return -1;
    }
    return ::write(fd, data, std::min(size, size_t(1)));
  }
};
NOVA_TEST(write_all_retries_interrupts_and_partial_writes, "io", "") {
  TempDirectory dir;
  InterruptedShortWriter io;
  std::vector<uint8_t> bytes = {4, 3, 2, 1};
  auto path = dir.path() / "short";
  CHECK(io.write_all(path, bytes, WriteMode::exclusive).ok);
  auto result = io.read(path, 4);
  CHECK(result);
  CHECK(result.value == bytes);
}
NOVA_TEST(posix_io_reports_permission_and_open_errors, "io", "") {
  TempDirectory dir;
  PosixFileIO io;
  auto path = dir.path() / "readonly";
  CHECK(io.write_all(path, {1}, WriteMode::exclusive).ok);
  CHECK(chmod(path.c_str(), 0400) == 0);
  auto denied = io.write_all(path, {2}, WriteMode::append);
  CHECK(!denied.ok);
  CHECK(denied.context.os_error == EACCES);
  CHECK(denied.code == ErrorCode::ioFailure);
  auto missing = io.read(dir.path() / "missing", 4);
  CHECK(missing.status.context.os_error == ENOENT);
}
NOVA_TEST(directory_lock_aliases_and_process_death, "io", "") {
  TempDirectory dir;
  auto real = dir.path() / "db";
  std::filesystem::create_directory(real);
  auto alias = dir.path() / "alias";
  std::filesystem::create_directory_symlink(real, alias);
  {
    auto first = DatabaseLock::acquire(real);
    CHECK(first);
    auto other = DatabaseLock::acquire(alias);
    CHECK(!other);
    CHECK(other.status.code == ErrorCode::busy);
  }
  int ready[2];
  CHECK(pipe(ready) == 0);
  auto pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    auto lock = DatabaseLock::acquire(real);
    char result = lock ? '1' : '0';
    write(ready[1], &result, 1);
    if (!lock)
      _exit(2);
    for (;;)
      pause();
  }
  close(ready[1]);
  char result = 0;
  CHECK(read(ready[0], &result, 1) == 1);
  close(ready[0]);
  CHECK(result == '1');
  CHECK(kill(pid, SIGKILL) == 0);
  int status;
  CHECK(waitpid(pid, &status, 0) == pid);
  auto acquired = DatabaseLock::acquire(alias);
  CHECK(acquired);
}
NOVA_TEST(query_context_distinguishes_deadline_and_cancellation, "io", "") {
  QueryContext query;
  CHECK(query.check().ok);
  query.cancel();
  CHECK(query.check().code == ErrorCode::cancelled);
  QueryContext timed(std::chrono::steady_clock::now() - std::chrono::seconds(1));
  CHECK(timed.check().code == ErrorCode::deadlineExceeded);
}
#include "ModelFileIO.hpp"
NOVA_TEST(atomic_catalog_faults_preserve_durable_predecessor, "io", "") {
  for (const auto &operation : {"write", "sync", "rename", "directorySync"}) {
    auto io = std::make_shared<ModelFileIO>();
    TempDirectory dir;
    auto path = dir.path() / "catalog";
    CHECK(Catalog::save(path, 1, {}, {}, io).ok);
    io->fail_next = operation;
    CHECK(!Catalog::save(path, 2, {}, {}, io).ok);
    io->power_loss();
    auto recovered = Catalog::load(path, io);
    CHECK(recovered);
    CHECK(recovered.value.last_lsn == 1);
    CHECK(Catalog::save(path, 3, {}, {}, io).ok);
    io->power_loss();
    CHECK(Catalog::load(path, io).value.last_lsn == 3);
  }
}
NOVA_TEST(uncertain_wal_failure_fences_mutation_and_checkpoint, "io", "") {
  for (const auto &operation : {"write", "sync"}) {
    TempDirectory dir;
    auto io = std::make_shared<ModelFileIO>();
    auto path = dir.path() / "wal";
    auto wal = std::make_shared<WalManager>(path, io);
    CHECK(wal->open().ok);
    auto hot = std::make_shared<MemoryHotStore>();
    auto disk = std::make_shared<FailingDiskStore>();
    EngineConfig config;
    config.database_dir = dir.path();
    config.file_io = io;
    {
      GraphEngine engine(hot, disk, config, wal);
      io->fail_next = operation;
      auto failed = engine.upsert_node("N", "a", {});
      CHECK(!failed.ok);
      CHECK(failed.code == ErrorCode::commitOutcomeUnknown);
      CHECK(!failed.context.transaction_id.empty());
      CHECK(!engine.get_node("a"));
      CHECK(engine.upsert_node("N", "b", {}).code == ErrorCode::closed);
      CHECK(engine.checkpoint().code == ErrorCode::closed);
    }
    CHECK(io->read(dir.path()/"catalog.db", 1024).status.code == ErrorCode::notFound);
    io->power_loss();
    WalManager reopened(path, io);
    CHECK(reopened.open().ok);
    auto recovered = reopened.recover_entries();
    CHECK(recovered);
    CHECK(recovered.value.empty());
  }
}
class ShortReader : public PosixFileIO {
  ssize_t read_some(int, void *, size_t) override { return 0; }
};
class FullDisk : public PosixFileIO {
  ssize_t write_some(int, const void *, size_t) override {
    errno = ENOSPC;
    return -1;
  }
};
NOVA_TEST(posix_short_read_and_disk_full_are_errors, "io", "") {
  TempDirectory dir;
  auto path = dir.path() / "file";
  PosixFileIO io;
  CHECK(io.write_all(path, {1, 2}, WriteMode::exclusive).ok);
  ShortReader reader;
  auto short_read = reader.read(path, 2);
  CHECK(!short_read);
  CHECK(short_read.status.context.os_error == EIO);
  FullDisk full;
  auto failed = full.write_all(path, {3}, WriteMode::append);
  CHECK(!failed.ok);
  CHECK(failed.context.os_error == ENOSPC);
}
NOVA_TEST(c_open_transports_busy_and_unsupported_errors, "io", "") {
  TempDirectory dir;
  {
    Handle first(dir.path());
    auto second = graphdb_open_result(dir.path().c_str(), 10, 10);
    CHECK(!second.handle);
    auto error = take(second.error);
    CHECK(error.find("\"code\":\"busy\"") != std::string::npos);
  }
  TempDirectory legacy;
  {
    std::ofstream out(legacy.path() / "catalog.db");
    out << "legacy";
  }
  auto rejected = graphdb_open_result(legacy.path().c_str(), 10, 10);
  CHECK(!rejected.handle);
  CHECK(take(rejected.error).find("\"code\":\"corruptData\"") != std::string::npos);
  CHECK(!std::filesystem::exists(legacy.path() / "FORMAT"));
  CHECK(!std::filesystem::exists(legacy.path() / "wal.log"));
}
NOVA_TEST(c_query_preserves_typed_not_found_and_parse_errors, "io", "") {
  TempDirectory dir;
  Handle db(dir.path());
  CHECK(db.query("get node absent").find("\"code\":\"notFound\"") != std::string::npos);
  CHECK(db.query("garbage").find("\"code\":\"parseError\"") != std::string::npos);
  CHECK(db.query("upsert node N a set v=$missing").find("\"code\":\"unboundParameter\"") !=
        std::string::npos);
}
NOVA_TEST(missing_wal_is_not_silently_recreated, "io", "") {
  TempDirectory dir;
  {
    Handle db(dir.path());
    CHECK(db.query("upsert node N a").find("\"ok\":true") != std::string::npos);
  }
  std::filesystem::remove(active_wal(dir.path()));
  auto result = graphdb_open_result(dir.path().c_str(), 10, 10);
  if (result.handle)
    graphdb_close(result.handle);
  CHECK(!result.handle);
  CHECK(take(result.error).find("\"code\":\"corruptData\"") != std::string::npos);
  CHECK(!std::filesystem::exists(active_wal(dir.path())));
}
NOVA_TEST(cold_traversal_propagates_read_failure, "io", "") {
  EngineFixture f;
  CHECK(f.engine->execute_dsl("upsert node N a; upsert node N b; upsert edge E a -> b").ok);
  f.cold(object_key(ObjectKind::Edge, edge_id("a", "E", "b")));
  auto broken = std::make_shared<FailingDiskStore>();
  GraphEngine engine(f.hot, broken, f.config);
  CHECK(engine.walk_out("a", "E", 1, 10).status.code == ErrorCode::ioFailure);
  CHECK(engine.walk_in("b", "E", 1, 10).status.code == ErrorCode::ioFailure);
}
NOVA_TEST(missing_checkpoint_catalog_rejects_open_preserving_wal, "io", "") {
  TempDirectory dir;
  {
    Handle db(dir.path());
    CHECK(db.query("upsert node N a").find("\"ok\":true") != std::string::npos);
  }
  PosixFileIO io;
  auto before = io.read(active_wal(dir.path()), 1024);
  CHECK(before);
  std::filesystem::remove(current_catalog(dir.path()));
  auto result = graphdb_open_result(dir.path().c_str(), 10, 10);
  if (result.handle)
    graphdb_close(result.handle);
  CHECK(!result.handle);
  CHECK(take(result.error).find("\"code\":\"corruptData\"") != std::string::npos);
  CHECK(!std::filesystem::exists(current_catalog(dir.path())));
  CHECK(io.read(active_wal(dir.path()), 1024).value == before.value);
}
NOVA_TEST(wal_capacity_rejection_still_allows_checkpoint, "io", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  auto wal = std::make_shared<WalManager>(dir.path() / "wal", io);
  CHECK(wal->open().ok);
  auto hot = std::make_shared<MemoryHotStore>();
  auto disk = std::make_shared<FailingDiskStore>();
  EngineConfig config;
  config.database_dir = dir.path();
  config.file_io = io;
  GraphEngine engine(hot, disk, config, wal);
  PropertyMap props = {{"value", std::string(1024 * 1024, 'x')}};
  Status outcome;
  for (int i = 0; i < 65 && outcome.ok; ++i)
    outcome = engine.upsert_node("N", "a", props);
  CHECK(outcome.code == ErrorCode::limitExceeded);
  CHECK(wal->writable());
  CHECK(engine.checkpoint().ok);
  CHECK(engine.upsert_node("N", "a", {}).ok);
}
NOVA_TEST(wal_recovers_since_initial_current_root, "io", "") {
 TempDirectory dir;auto recovery=Recovery::open(dir.path());CHECK(recovery);CHECK(recovery.value->wal()->append_upsert_node("N","a",{}));recovery.value.reset();Handle db(dir.path());CHECK(db.query("get node a").find("\"ok\":true")!=std::string::npos);
}
NOVA_TEST(missing_current_rejects_orphan_generations, "io", "") {
 TempDirectory dir;{Handle db(dir.path());CHECK(db.query("upsert node N a").find("\"ok\":true")!=std::string::npos);}std::filesystem::remove(dir.path()/"CURRENT");auto opened=graphdb_open_result(dir.path().c_str(),10,10);CHECK(!opened.handle);CHECK(take(opened.error).find("corruptData")!=std::string::npos);CHECK(!std::filesystem::exists(dir.path()/"CURRENT"));
}
NOVA_TEST(directory_owner_rejects_another_process, "io", "") {
  TempDirectory dir;
  auto lock = DatabaseLock::acquire(dir.path());
  CHECK(lock);
  auto child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    auto second = DatabaseLock::acquire(dir.path());
    _exit(!second && second.status.code == ErrorCode::busy ? 0 : 1);
  }
  int status;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

NOVA_TEST(sequential_reader_bounds_reads_and_handles_short_io, "io", "") {
  class ShortIO:public PosixFileIO {
    bool interrupt=true;
    ssize_t read_some(int fd,void *data,size_t size) override {
      if(interrupt){interrupt=false;errno=EINTR;return -1;}
      return PosixFileIO::read_some(fd,data,std::min(size,size_t(7)));
    }
  } io;
  TempDirectory dir;auto path=dir.path()/"stream";
  std::vector<uint8_t> bytes(40000);for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(i);
  CHECK(io.write_all(path,bytes,WriteMode::exclusive).ok);
  CHECK(!io.open_reader(path,100,16));
  auto reader=io.open_reader(path,65536,16);CHECK(reader);CHECK(reader.value->size()==bytes.size());
  std::array<uint8_t,19> buffer;size_t offset=0;
  while(offset<bytes.size()) {auto r=reader.value->read(buffer.data(),buffer.size());CHECK(r);CHECK(r.value>0 && r.value<=buffer.size());
    for(size_t i=0;i<r.value;++i)CHECK(buffer[i]==bytes[offset+i]);offset+=r.value;}
  auto end=reader.value->read(buffer.data(),buffer.size());CHECK(end && end.value==0);
  CHECK(reader.value->close().ok);
  auto short_file=io.open_reader(path,65536,16);CHECK(short_file);CHECK(io.truncate(path,1).ok);
  auto first=short_file.value->read(buffer.data(),buffer.size());CHECK(first && first.value==1);
  CHECK(!short_file.value->read(buffer.data(),buffer.size()));
}

NOVA_TEST(sequential_reader_fallback_rejects_unreserved_large_file, "io", "") {
  class FallbackIO:public ModelFileIO {
  public:
    Result<std::unique_ptr<SequentialReader>> open_reader(const std::filesystem::path &p,size_t maximum,size_t fallback) override {
      return FileIO::open_reader(p,maximum,fallback);
    }
  } io;
  CHECK(io.write_all("small",{1,2,3},WriteMode::exclusive).ok);
  CHECK(io.write_all("large",std::vector<uint8_t>(65536),WriteMode::exclusive).ok);
  CHECK(!io.open_reader("large",65536,16384));
  auto reader=io.open_reader("small",65536,16384);CHECK(reader);
  std::array<uint8_t,3> bytes{};auto r=reader.value->read(bytes.data(),bytes.size());CHECK(r && r.value==3);CHECK(bytes[2]==3);
}

NOVA_TEST(trim_later_posix_flush_failure_keeps_completed_prefix, "io", "") {
  class LaterSyncIO : public PosixFileIO {
  public:
    size_t groups=0;bool in_group=false;
    Status sync_files(const std::vector<std::filesystem::path> &paths) override {
      ++groups;in_group=true;auto result=PosixFileIO::sync_files(paths);in_group=false;return result;
    }
  protected:
    int sync_descriptor(int fd,bool full) override {
#ifdef __APPLE__
      bool selected=full;
#else
      bool selected=!full;
#endif
      if(in_group && groups==2 && selected){errno=EIO;return -1;}
      return PosixFileIO::sync_descriptor(fd,full);
    }
  };
  TempDirectory dir;auto io=std::make_shared<LaterSyncIO>();
  auto hot=std::make_shared<MemoryHotStore>();
  auto disk=std::make_shared<FileDiskStore>(dir.path()/"cold",io);
  for(size_t i=0;i<70;++i){GraphObject o;o.id=std::to_string(i);o.label_or_type="N";CHECK(hot->put_hot("n/"+o.id,o).ok);}
  EngineConfig config;config.database_dir=dir.path();config.file_io=io;config.checkpoint_on_destroy=false;
  GraphEngine engine(hot,disk,config);QueryContext context;
  auto result=engine.trim_memory(0,context);
  CHECK(!result && result.status.code==ErrorCode::ioFailure);
  CHECK(result.value.evicted_count==64 && hot->hot_count()==6);
  for(const auto &record:hot->dump_records())if(record.tier==StorageTier::Cold)CHECK(read_manifest(*disk,record));
  QueryContext retry;CHECK(engine.trim_memory(0,retry));CHECK(hot->hot_count()==0);
}
