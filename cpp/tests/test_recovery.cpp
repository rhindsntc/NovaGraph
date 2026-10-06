#include "TestSupport.hpp"
#include "faults/FaultIO.hpp"
using namespace graphdb;
using namespace nova_test;
NOVA_TEST(recovery_fault_inventory_covers_durable_phases, "recovery", "") {
  TempDirectory dir;
  auto io = std::make_shared<FaultIO<ModelFileIO>>();
  auto root = Recovery::open(dir.path(), io);
  CHECK(root);
  EngineConfig config;
  config.database_dir = dir.path();
  config.file_io = io;
  config.checkpoint_on_destroy = false;
  auto hot = std::make_shared<MemoryHotStore>();
  auto disk = std::make_shared<FileDiskStore>(dir.path() / "cold", io);
  GraphEngine engine(hot, disk, config, nullptr, root.value);
  CHECK(engine.recover().ok);
  io->armed = true;
  CHECK(engine.upsert_node("N", "a", {}).ok);
  QueryContext ctx;
  CHECK(engine.trim_memory(0, ctx));
  CHECK(engine.checkpoint().ok);
  CHECK(engine.delete_node("a").ok);
  CHECK(engine.checkpoint().ok);
  CHECK(engine.checkpoint().ok);
  CHECK(engine.collect_garbage(100, ctx));
  for (const auto &point :
       {"wal.after_bytes", "wal.after_commit", "wal.after_sync", "transaction.before_publication",
        "transaction.after_publication", "payload.after_write", "payload.after_sync",
        "payload.after_residency_switch", "catalog.after_publish", "root.after_publish",
        "wal.after_rotation", "gc.after_mark", "gc.after_remove", "gc.after_directory_sync"})
    CHECK(io->events.count(point));
}
#include "faults/Campaign.hpp"
NOVA_TEST(modeled_power_loss_at_every_small_graph_boundary, "recovery", "") {
  auto baseline = model_trace(0, 0);
  CHECK(baseline.events > 50);
  for (size_t point = 1; point <= baseline.events; ++point) {
    try {
      auto result = model_trace(0, point);
      CHECK(result.crashed);
    } catch (const std::exception &e) {
      throw Failure("fault=" + std::to_string(point) + " " + e.what());
    }
  }
}
NOVA_TEST(invalid_root_and_retained_catalog_preserve_wal_tail_evidence, "recovery", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  auto root = Recovery::open(dir.path(), io);
  CHECK(root);
  EngineConfig config;
  config.database_dir = dir.path();
  config.file_io = io;
  config.checkpoint_on_destroy = false;
  auto hot = std::make_shared<MemoryHotStore>();
  auto disk = std::make_shared<FailingDiskStore>();
  GraphEngine engine(hot, disk, config, nullptr, root.value);
  CHECK(engine.recover().ok);
  CHECK(engine.upsert_node("N", "a", {}).ok);
  CHECK(engine.checkpoint().ok);
  auto active = dir.path() / root.value->root().active.filename;
  CHECK(io->write_all(active, {4, 0}, WriteMode::append).ok);
  auto wal = io->read(active, 10000).value;
  auto root_bytes = io->read(dir.path() / "CURRENT", 10000).value;
  for (size_t cut = 0; cut < root_bytes.size(); ++cut) {
    auto broken = std::vector<uint8_t>(root_bytes.begin(), root_bytes.begin() + cut);
    CHECK(io->write_all(dir.path() / "CURRENT", broken, WriteMode::replace).ok);
    CHECK(!Recovery::open(dir.path(), io));
    CHECK(io->read(dir.path() / "CURRENT", 10000).value == broken);
    CHECK(io->read(active, 10000).value == wal);
  }
  auto unknown = root_bytes;
  unknown[4] = 99;
  CHECK(io->write_all(dir.path() / "CURRENT", unknown, WriteMode::replace).ok);
  CHECK(Recovery::open(dir.path(), io).status.code == ErrorCode::unsupportedVersion);
  CHECK(io->read(active, 10000).value == wal);
  CHECK(io->write_all(dir.path() / "CURRENT", root_bytes, WriteMode::replace).ok);
  auto previous = dir.path() / root.value->root().previous->filename;
  CHECK(io->write_all(previous, {0, 1, 2}, WriteMode::replace).ok);
  CHECK(!Recovery::open(dir.path(), io));
  CHECK(io->read(active, 10000).value == wal);
}
NOVA_TEST(cold_missing_truncated_and_wrong_version_payloads_are_errors, "recovery", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N", "a", {{"v", int64_t(7)}}).ok);
  f.cold("n/a");
  auto record = f.hot->get("n/a");
  auto path = f.dir.path() / "cold" / record->disk_key;
  PosixFileIO io;
  auto original = io.read(path, kMaxObjectBytes).value;
  for (size_t cut = 0; cut < original.size(); ++cut) {
    auto broken = std::vector<uint8_t>(original.begin(), original.begin() + cut);
    CHECK(io.write_all(path, broken, WriteMode::replace).ok);
    CHECK(!f.engine->get_node("a"));
    CHECK(io.read(path, kMaxObjectBytes).value == broken);
  }
  auto wrong = original;
  wrong[4] = 99;
  CHECK(io.write_all(path, wrong, WriteMode::replace).ok);
  CHECK(f.engine->get_node("a").status.code == ErrorCode::unsupportedVersion);
  CHECK(io.remove(path).ok);
  CHECK(f.engine->get_node("a").status.code == ErrorCode::corruptData);
}
NOVA_TEST(index_section_enum_count_and_version_corruption_is_explicit, "recovery", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  CHECK(Catalog::save(dir.path() / "empty", CatalogData{}, io).ok);
  auto original = io->read(dir.path() / "empty", 10000).value;
  auto section = original.size() - 4 - 21;
  for (int kind = 0; kind < 3; ++kind) {
    auto bytes = original;
    if (kind == 0)
      bytes[section] = 99;
    else if (kind == 1)
      for (size_t i = 0; i < 4; ++i)
        bytes[section + 13 + i] = 255;
    else
      bytes[section + 1] = 99;
    auto set_crc = [&](size_t offset, size_t start) {
      auto crc = crc32_compute(bytes.data() + start, offset - start);
      for (int i = 0; i < 4; ++i)
        bytes[offset + i] = uint8_t(crc >> (8 * i));
    };
    set_crc(bytes.size() - 8, section);
    set_crc(bytes.size() - 4, 0);
    CHECK(io->write_all(dir.path() / "empty", bytes, WriteMode::replace).ok);
    auto loaded = Catalog::load(dir.path() / "empty", io);
    CHECK(!loaded);
    CHECK(loaded.status.code ==
          (kind == 2 ? ErrorCode::unsupportedVersion : ErrorCode::corruptData));
    CHECK(io->read(dir.path() / "empty", 10000).value == bytes);
  }
}
NOVA_TEST(recovery_respects_hot_budget_after_uncheckpointed_trims, "recovery", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  EngineConfig config;
  config.database_dir = dir.path();
  config.file_io = io;
  config.checkpoint_on_destroy = false;
  config.memory.hot_payload_bytes = 1000;
  auto disk = std::make_shared<FileDiskStore>(dir.path() / "cold", io);
  // Initialize before the disk directory exists only in the modeled namespace.
  {
    auto root = Recovery::open(dir.path(), io);
    CHECK(root);
    auto hot = std::make_shared<MemoryHotStore>();
    GraphEngine engine(hot, disk, config, nullptr, root.value);
    CHECK(engine.recover().ok);
    QueryContext ctx;
    for (int i = 0; i < 8; ++i) {
      CHECK(engine.upsert_node("N", std::to_string(i), {{"v", std::string(600, 'x')}}).ok);
      CHECK(engine.trim_memory(0, ctx));
    }
  }
  io->power_loss();
  auto opened = Recovery::open(dir.path(), io);
  CHECK(opened);
  auto hot = std::make_shared<MemoryHotStore>();
  GraphEngine recovered(hot, disk, config, nullptr, opened.value);
  CHECK(recovered.recover().ok);
  CHECK(recovered.memory_usage().hot_payload_bytes <= 1000);
  for (int i = 0; i < 8; ++i)
    CHECK(recovered.get_node(std::to_string(i)));
  CHECK(recovered.memory_usage().hot_payload_bytes <= 1000);
}
NOVA_TEST(wal_replay_preserves_node_edge_modification_time, "recovery", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  EngineConfig config;
  config.database_dir = dir.path();
  config.file_io = io;
  config.checkpoint_on_destroy = false;
  int64_t clock = 12345;
  config.wall_clock = [&] { return clock; };
  auto disk = std::make_shared<FailingDiskStore>();
  {
    auto root = Recovery::open(dir.path(), io);
    CHECK(root);
    auto hot = std::make_shared<MemoryHotStore>();
    GraphEngine engine(hot, disk, config, nullptr, root.value);
    CHECK(engine.recover().ok);
    CHECK(engine.execute_dsl("upsert node N a; upsert node N b; upsert edge E a -> b").ok);
    clock = -7;
    CHECK(engine.upsert_node("N", "a", {}).ok);
  }
  clock = 54321;
  io->power_loss();
  auto root = Recovery::open(dir.path(), io);
  CHECK(root);
  auto hot = std::make_shared<MemoryHotStore>();
  GraphEngine after(hot, disk, config, nullptr, root.value);
  CHECK(after.recover().ok);
  CHECK(after.get_node("a").value.last_modified_ms == -7);
  CHECK(after.get_node("b").value.last_modified_ms == 12345);
  CHECK(after.get_edge("a", "E", "b").value.last_modified_ms == 12345);
  CHECK(after.checkpoint().ok);
  clock = 0;
  io->power_loss();
  auto again = Recovery::open(dir.path(), io);
  CHECK(again);
  auto final_hot = std::make_shared<MemoryHotStore>();
  GraphEngine final(final_hot, disk, config, nullptr, again.value);
  CHECK(final.recover().ok);
  CHECK(final.get_node("a").value.last_modified_ms == -7);
  CHECK(final.get_edge("a", "E", "b").value.last_modified_ms == 12345);
}
NOVA_TEST(failed_recovery_spill_preserves_committed_evidence, "recovery", "") {
  for (const auto *operation : {"write", "sync", "directorySync"}) {
    TempDirectory dir;
    auto io = std::make_shared<ModelFileIO>();
    EngineConfig config;
    config.database_dir = dir.path();
    config.file_io = io;
    config.checkpoint_on_destroy = false;
    config.memory.hot_payload_bytes = 1000;
    auto disk = std::make_shared<FileDiskStore>(dir.path() / "cold", io);
    {
      auto root = Recovery::open(dir.path(), io);
      CHECK(root);
      auto hot = std::make_shared<MemoryHotStore>();
      GraphEngine engine(hot, disk, config, nullptr, root.value);
      CHECK(engine.recover().ok);
      QueryContext ctx;
      CHECK(engine.upsert_node("N", "a", {{"v", std::string(600, 'x')}}).ok);
      CHECK(engine.trim_memory(0, ctx));
      CHECK(engine.upsert_node("N", "b", {{"v", std::string(600, 'x')}}).ok);
    }
    auto opened = Recovery::open(dir.path(), io);
    CHECK(opened);
    auto path = dir.path() / opened.value->root().active.filename;
    auto before = io->read(path, 10000).value;
    auto root_before = io->read(dir.path() / "CURRENT", 10000).value;
    auto hot = std::make_shared<MemoryHotStore>();
    GraphEngine failed(hot, disk, config, nullptr, opened.value);
    io->fail_next = operation;
    CHECK(!failed.recover().ok);
    CHECK(hot->hot_count() == 0 && hot->cold_count() == 0);
    CHECK(io->read(path, 10000).value == before);
    CHECK(io->read(dir.path() / "CURRENT", 10000).value == root_before);
    io->power_loss();
    auto reopened = Recovery::open(dir.path(), io);
    CHECK(reopened);
    auto next = std::make_shared<MemoryHotStore>();
    GraphEngine recovered(next, disk, config, nullptr, reopened.value);
    CHECK(recovered.recover().ok);
    CHECK(recovered.get_node("a") && recovered.get_node("b"));
  }
}

namespace {
// Exercise real fsync/close paths; inject only the selected descriptor failure.
class CheckpointSyncIO : public PosixFileIO {
public:
  size_t full_syncs{0}, members{0}, fail_member{0}, fail_flush_group{0};
  std::vector<size_t> groups;
  std::set<std::filesystem::path> payloads;
  bool in_group{false}, fail_catalog{false}, fail_root{false};
  Status write_all(const std::filesystem::path &path,const std::vector<uint8_t> &bytes,WriteMode mode) override {
    auto name=path.filename().string();
    if((fail_catalog && name.starts_with("catalog-")) || (fail_root && name=="CURRENT.tmp"))
      return Status::Error("injected checkpoint publication failure",ErrorCode::ioFailure);
    return PosixFileIO::write_all(path,bytes,mode);
  }
  Status sync_files(const std::vector<std::filesystem::path> &paths) override {
    groups.push_back(paths.size());
    for (const auto &path : paths) payloads.insert(path);
    in_group=true;
    auto status=PosixFileIO::sync_files(paths);
    in_group=false;
    return status;
  }
  void reset() {full_syncs=members=0;groups.clear();payloads.clear();}
protected:
  int sync_descriptor(int fd,bool full) override {
    if(full)++full_syncs;
    if(in_group && !full && ++members==fail_member) {errno=EIO;return -1;}
    if(in_group && full && groups.size()==fail_flush_group) {errno=EIO;return -1;}
    return PosixFileIO::sync_descriptor(fd,full);
  }
};
struct CheckpointSyncFixture {
  TempDirectory dir;
  std::shared_ptr<CheckpointSyncIO> io=std::make_shared<CheckpointSyncIO>();
  std::shared_ptr<MemoryHotStore> hot=std::make_shared<MemoryHotStore>();
  std::shared_ptr<Recovery> recovery;
  std::unique_ptr<GraphEngine> engine;
  CheckpointSyncFixture() {
    auto opened=Recovery::open(dir.path(),io);CHECK(opened);recovery=opened.value;
    EngineConfig config;config.database_dir=dir.path();config.file_io=io;
    config.checkpoint_on_destroy=false;
    engine=std::make_unique<GraphEngine>(hot,std::make_shared<FileDiskStore>(dir.path()/"cold",io),config,nullptr,recovery);
    CHECK(engine->recover().ok);
    TransactionBatch batch{new_transaction_id(),{}};
    for(size_t i=0;i<130;++i)batch.mutations.push_back(UpsertNode{"N","n"+std::to_string(i),{{"v",int64_t(i)}}});
    QueryContext context;CHECK(engine->commit_batch(batch,context));
    CHECK(engine->trim_memory(0,context));
    io->reset();
  }
};
}
NOVA_TEST(checkpoint_groups_cold_syncs_before_publishing_recoverable_root, "recovery", "") {
  CheckpointSyncFixture f;
  CHECK(f.engine->checkpoint().ok);
  // 130 payloads must not require 130 full-device flushes; allow fixed root/WAL IO.
  CHECK(f.io->full_syncs<16);
  CHECK(f.io->members==130);
  CHECK(f.io->payloads.size()==130);
  CHECK(!f.io->groups.empty());
  for(auto size:f.io->groups)CHECK(size>0 && size<=64);
  f.engine.reset();f.recovery.reset();
  auto reopened=Recovery::open(f.dir.path(),f.io);CHECK(reopened);
  CHECK(reopened.value->catalog().records.size()==130);
  GraphEngine after(std::make_shared<MemoryHotStore>(),std::make_shared<FileDiskStore>(f.dir.path()/"cold",f.io),
                    [&]{EngineConfig c;c.database_dir=f.dir.path();c.file_io=f.io;c.checkpoint_on_destroy=false;return c;}(),nullptr,reopened.value);
  CHECK(after.recover().ok);
  for(size_t i=0;i<130;++i)CHECK(after.get_node("n"+std::to_string(i)).value.properties.at("v")==Property(int64_t(i)));
}
NOVA_TEST(checkpoint_group_sync_failure_preserves_root_wal_and_retry, "recovery", "") {
  for(size_t failure: {size_t(1),size_t(70),size_t(130),size_t(131),size_t(132)}) {
#ifndef __APPLE__
    if(failure>=131)continue; // Other POSIX platforms do not issue a shared full flush.
#endif
    CheckpointSyncFixture f;
    auto root=f.io->read(f.dir.path()/"CURRENT",10000);CHECK(root);
    auto walpath=f.dir.path()/f.recovery->root().active.filename;
    auto wal=f.io->read(walpath,1024*1024);CHECK(wal);
    if(failure>=131)f.io->fail_flush_group=failure-129;else f.io->fail_member=failure;
    auto failed=f.engine->checkpoint();CHECK(!failed.ok);CHECK(failed.code==ErrorCode::ioFailure);
    CHECK(f.io->read(f.dir.path()/"CURRENT",10000).value==root.value);
    CHECK(f.io->read(walpath,1024*1024).value==wal.value);
    CHECK(!f.recovery->fenced());
    // Recovery still reconstructs all acknowledged values from the previous root/WAL.
    auto reopened=Recovery::open(f.dir.path(),f.io);CHECK(reopened);
    CHECK(reopened.value->transactions().size()==1);
    f.io->fail_member=f.io->fail_flush_group=0;f.io->reset();
    CHECK(f.engine->checkpoint().ok);
    CHECK(Recovery::open(f.dir.path(),f.io).value->catalog().records.size()==130);
  }
}

NOVA_TEST(checkpoint_unchanged_payloads_remain_durable_without_resync_after_reopen, "recovery", "") {
  CheckpointSyncFixture f;
  CHECK(f.engine->checkpoint().ok);
  auto generation=f.recovery->root().generation;
  f.io->reset();
  CHECK(f.engine->checkpoint().ok);
  CHECK(f.io->members==0 && f.io->groups.empty());
  CHECK(f.recovery->root().generation==generation+1);
  f.engine.reset();f.recovery.reset();
  auto opened=Recovery::open(f.dir.path(),f.io);CHECK(opened);
  f.io->reset();
  CHECK(opened.value->checkpoint(opened.value->catalog()).ok);
  CHECK(f.io->members==0 && f.io->groups.empty());
  auto reopened=Recovery::open(f.dir.path(),f.io);CHECK(reopened);
  FileDiskStore disk(f.dir.path()/"cold",f.io);
  for(const auto &record:reopened.value->catalog().records) {
    auto value=disk.read(record.disk_key);CHECK(value);
    CHECK(value.value.id==record.object.id);
    CHECK(value.value.properties.at("v")==Property(int64_t(std::stoll(record.object.id.substr(1)))));
  }
}

NOVA_TEST(checkpoint_syncs_only_new_replaced_and_retrimmed_payloads, "recovery", "") {
  CheckpointSyncFixture f;CHECK(f.engine->checkpoint().ok);
  CHECK(f.engine->upsert_node("N","a-new",{{"v",int64_t(999)}}).ok);
  CHECK(f.engine->upsert_node("N","n0",{{"v",int64_t(888)}}).ok);
  CHECK(f.engine->delete_node("n64").ok);
  QueryContext trim;CHECK(f.engine->trim_memory(0,trim));
  f.io->reset();CHECK(f.engine->checkpoint().ok);
  CHECK(f.io->members==2 && f.io->payloads.size()==2);
  CHECK(f.engine->get_node("n1"));
  f.io->reset();CHECK(f.engine->checkpoint().ok);
  CHECK(f.io->members==0);
  QueryContext retrim;CHECK(f.engine->trim_memory(0,retrim));
  f.io->reset();CHECK(f.engine->checkpoint().ok);
  CHECK(f.io->members==1);
  QueryContext gc;CHECK(f.engine->collect_garbage(4096,gc));
  auto opened=Recovery::open(f.dir.path(),f.io);CHECK(opened);
  FileDiskStore disk(f.dir.path()/"cold",f.io);
  for(const auto &record:opened.value->catalog().records)CHECK(disk.read(record.disk_key));
  CHECK(f.engine->get_node("a-new").value.properties.at("v")==Property(int64_t(999)));
  CHECK(f.engine->get_node("n0").value.properties.at("v")==Property(int64_t(888)));
  CHECK(!f.engine->get_node("n64"));
}

NOVA_TEST(checkpoint_unordered_manifests_conservatively_sync_all_payloads, "recovery", "") {
  CheckpointSyncFixture f;CHECK(f.engine->checkpoint().ok);
  auto data=f.recovery->catalog();std::reverse(data.records.begin(),data.records.end());
  f.io->reset();CHECK(f.recovery->checkpoint(data).ok);CHECK(f.io->members==130);
  // An unordered previously published catalog also forces conservative sync.
  std::reverse(data.records.begin(),data.records.end());
  f.io->reset();CHECK(f.recovery->checkpoint(data).ok);CHECK(f.io->members==130);
  f.io->reset();CHECK(f.recovery->checkpoint(data).ok);CHECK(f.io->members==0);
}

NOVA_TEST(checkpoint_changed_version_extent_or_checksum_cannot_reuse_durability, "recovery", "") {
  for(int field:{0,1,2}) {
    CheckpointSyncFixture f;CHECK(f.engine->checkpoint().ok);
    auto data=f.recovery->catalog();
    if(field==0)++data.records[0].payload_crc;
    else if(field==1)++data.records[0].payload_bytes;
    else {
      ++data.records[0].object.version;
      MemoryHotStore expected;expected.load_records(data.records,data.declared_indexes);
      data.indexes=expected.dump_indexes();
    }
    f.io->reset();CHECK(f.recovery->checkpoint(data).ok);
    CHECK(f.io->members==1);
  }
}

NOVA_TEST(checkpoint_failed_new_payload_sync_does_not_become_durable_knowledge, "recovery", "") {
  CheckpointSyncFixture f;CHECK(f.engine->checkpoint().ok);
  CHECK(f.engine->upsert_node("N","a-new",{{"v",int64_t(77)}}).ok);
  QueryContext trim;CHECK(f.engine->trim_memory(0,trim));
  auto root=f.io->read(f.dir.path()/"CURRENT",10000);CHECK(root);
  f.io->reset();f.io->fail_member=1;
  CHECK(!f.engine->checkpoint().ok);
  CHECK(f.io->read(f.dir.path()/"CURRENT",10000).value==root.value);
  CHECK(f.recovery->catalog().records.size()==130);
  CHECK(!f.recovery->fenced());
  f.io->fail_member=0;f.io->reset();CHECK(f.engine->checkpoint().ok);
  CHECK(f.io->members==1);
  auto reopened=Recovery::open(f.dir.path(),f.io);CHECK(reopened);
  CHECK(reopened.value->catalog().records.size()==131);
}

NOVA_TEST(checkpoint_reused_payload_corruption_remains_a_read_error, "recovery", "") {
  for(bool missing:{false,true}) {
    CheckpointSyncFixture f;CHECK(f.engine->checkpoint().ok);
    auto record=f.recovery->catalog().records.front();
    auto path=f.dir.path()/"cold"/record.disk_key;
    if(missing)CHECK(f.io->remove(path).ok);
    else CHECK(f.io->write_all(path,{1,2,3},WriteMode::replace).ok);
    f.io->reset();CHECK(f.engine->checkpoint().ok);CHECK(f.io->members==0);
    auto value=f.engine->get_node(record.object.id);
    CHECK(!value && value.status.code==ErrorCode::corruptData);
  }
}

NOVA_TEST(checkpoint_catalog_failure_retries_new_payload_sync, "recovery", "") {
  CheckpointSyncFixture f;CHECK(f.engine->checkpoint().ok);
  CHECK(f.engine->upsert_node("N","a-new",{}).ok);
  QueryContext trim;CHECK(f.engine->trim_memory(0,trim));
  auto generation=f.recovery->root().generation;
  f.io->reset();f.io->fail_catalog=true;
  CHECK(!f.engine->checkpoint().ok);CHECK(f.io->members==1);
  CHECK(f.recovery->root().generation==generation);
  CHECK(f.recovery->catalog().records.size()==130);
  CHECK(!f.recovery->fenced());
  f.io->fail_catalog=false;f.io->reset();CHECK(f.engine->checkpoint().ok);
  CHECK(f.io->members==1);
  CHECK(Recovery::open(f.dir.path(),f.io).value->catalog().records.size()==131);
}

NOVA_TEST(checkpoint_root_failure_fences_without_adopting_new_payloads, "recovery", "") {
  CheckpointSyncFixture f;CHECK(f.engine->checkpoint().ok);
  CHECK(f.engine->upsert_node("N","a-new",{{"v",int64_t(77)}}).ok);
  QueryContext trim;CHECK(f.engine->trim_memory(0,trim));
  auto generation=f.recovery->root().generation;
  f.io->reset();f.io->fail_root=true;
  CHECK(!f.engine->checkpoint().ok);CHECK(f.io->members==1);
  CHECK(f.recovery->root().generation==generation);
  CHECK(f.recovery->catalog().records.size()==130);
  CHECK(f.recovery->fenced());
  f.io->fail_root=false;CHECK(!f.engine->checkpoint().ok);
  auto opened=Recovery::open(f.dir.path(),f.io);CHECK(opened);
  CHECK(opened.value->transactions().size()==1);
  EngineConfig config;config.database_dir=f.dir.path();config.file_io=f.io;config.checkpoint_on_destroy=false;
  GraphEngine after(std::make_shared<MemoryHotStore>(),std::make_shared<FileDiskStore>(f.dir.path()/"cold",f.io),config,nullptr,opened.value);
  CHECK(after.recover().ok);
  CHECK(after.get_node("a-new").value.properties.at("v")==Property(int64_t(77)));
  CHECK(after.checkpoint().ok);
}

NOVA_TEST(checkpoint_mixed_node_edge_manifests_reuse_only_exact_identities, "recovery", "") {
  CheckpointSyncFixture f;CHECK(f.engine->checkpoint().ok);
  CHECK(f.engine->upsert_edge("LINK","n0","n1",{}).ok);
  QueryContext trim;CHECK(f.engine->trim_memory(0,trim));
  f.io->reset();CHECK(f.engine->checkpoint().ok);CHECK(f.io->members==1);
  f.io->reset();CHECK(f.engine->checkpoint().ok);CHECK(f.io->members==0);
  CHECK(f.engine->upsert_edge("LINK","n0","n1",{{"v",int64_t(99)}}).ok);
  QueryContext again;CHECK(f.engine->trim_memory(0,again));
  f.io->reset();CHECK(f.engine->checkpoint().ok);CHECK(f.io->members==1);
  auto opened=Recovery::open(f.dir.path(),f.io);CHECK(opened);
  CHECK(opened.value->catalog().records.size()==131);
}

NOVA_TEST(checkpoint_reused_payloads_survive_every_modeled_publication_boundary, "recovery", "") {
  auto trace=[](size_t crash_at) {
    TempDirectory dir;
    struct CountingFaultIO : FaultIO<ModelFileIO> {
      size_t synced=0;
      Status sync_files(const std::vector<std::filesystem::path> &paths) override {
        synced+=paths.size();
        return FaultIO<ModelFileIO>::sync_files(paths);
      }
    };
    auto io=std::make_shared<CountingFaultIO>();
    auto root=Recovery::open(dir.path(),io);CHECK(root);
    EngineConfig config;config.database_dir=dir.path();config.file_io=io;config.checkpoint_on_destroy=false;
    auto disk=std::make_shared<FileDiskStore>(dir.path()/"cold",io);
    bool crashed=false;
    {
      GraphEngine engine(std::make_shared<MemoryHotStore>(),disk,config,nullptr,root.value);
      CHECK(engine.recover().ok);
      CHECK(engine.upsert_node("N","kept-a",{{"v",int64_t(10)}}).ok);
      CHECK(engine.upsert_node("N","kept-b",{{"v",int64_t(20)}}).ok);
      CHECK(engine.upsert_edge("LINK","kept-a","kept-b",{{"v",int64_t(40)}}).ok);
      QueryContext trim;CHECK(engine.trim_memory(0,trim));CHECK(engine.checkpoint().ok);
      CHECK(engine.upsert_node("N","added",{{"v",int64_t(30)}}).ok);
      QueryContext again;CHECK(engine.trim_memory(0,again));
      io->synced=0;io->armed=true;io->crash_at=crash_at;
      try {
        CHECK(engine.checkpoint().ok);CHECK(io->synced==1);
        CHECK(engine.checkpoint().ok);CHECK(io->synced==1);
        QueryContext gc;CHECK(engine.collect_garbage(4096,gc));
      } catch(const PowerLoss &) {crashed=true;}
      io->armed=false;
    }
    CHECK(crashed==(crash_at!=0));
    auto events=io->counter;
    root.value.reset();io->power_loss();
    auto opened=Recovery::open(dir.path(),io);CHECK(opened);
    GraphEngine after(std::make_shared<MemoryHotStore>(),disk,config,nullptr,opened.value);
    CHECK(after.recover().ok);
    for(auto [id,value]:std::initializer_list<std::pair<const char *,int64_t>>{
          {"kept-a",10},{"kept-b",20},{"added",30}}) {
      auto record=after.get_node(id);CHECK(record);
      CHECK(record.value.properties.at("v")==Property(value));
    }
    auto edge=after.get_edge("kept-a","LINK","kept-b");CHECK(edge);
    CHECK(edge.value.properties.at("v")==Property(int64_t(40)));
    return events;
  };
  auto events=trace(0);CHECK(events>30);
  for(size_t point=1;point<=events;++point) {
    try {trace(point);}
    catch(const std::exception &e) {throw Failure("reuse fault="+std::to_string(point)+" "+e.what());}
  }
}
