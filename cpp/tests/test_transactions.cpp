#include "TestSupport.hpp"
#include "graphdb/QueryContext.hpp"
using namespace graphdb;
using namespace nova_test;
NOVA_TEST(batch_validates_staged_endpoints_and_rolls_back, "transactions", "") {
  EngineFixture f;
  QueryContext ctx;
  TransactionBatch batch{
      new_transaction_id(),
      {UpsertNode{"N", "a", {}}, UpsertNode{"N", "b", {}}, UpsertEdge{"E", "a", "b", {}}}};
  CHECK(f.engine->commit_batch(batch, ctx));
  auto bad = f.engine->execute_dsl("upsert node N c; upsert edge E c -> missing");
  CHECK(!bad.ok);
  CHECK(!f.engine->get_node("c"));
  CHECK(f.engine->get_edge("a", "E", "b"));
  auto cancelled = TransactionBatch{new_transaction_id(), {UpsertNode{"N", "d", {}}}};
  ctx.cancel();
  CHECK(f.engine->commit_batch(cancelled, ctx).status.code == ErrorCode::cancelled);
  CHECK(!f.engine->get_node("d"));
}
NOVA_TEST(program_parses_binds_and_rejects_maintenance_before_writing, "transactions", "") {
  EngineFixture f;
  for (const auto &query :
       {"upsert node N a; invalid syntax", "upsert node N a; upsert node N b set v=$missing",
        "upsert node N a; checkpoint", "upsert node N a; rebuild indexes"}) {
    CHECK(!f.engine->execute_dsl(query).ok);
    CHECK(!f.engine->get_node("a"));
  }
  CHECK(!f.engine->upsert_edge("E", "absent", "alsoAbsent", {}).ok);
  EngineConfig legacy = f.config;
  legacy.legacy_permissive_endpoints = true;
  GraphEngine permissive(f.hot, f.disk, legacy);
  CHECK(permissive.upsert_edge("E", "absent", "alsoAbsent", {}).ok);
}
NOVA_TEST(failed_program_remains_absent_after_checkpoint, "transactions", "") {
  EngineFixture f;
  CHECK(!f.engine->execute_dsl("upsert node N a; delete node missing").ok);
  CHECK(f.engine->checkpoint().ok);
  f.engine.reset();
  auto hot = std::make_shared<MemoryHotStore>();
  GraphEngine reopened(hot, f.disk, f.config);
  CHECK(reopened.recover().ok);
  CHECK(!reopened.get_node("a"));
}
#include "ModelFileIO.hpp"
NOVA_TEST(commit_sync_failure_publishes_nothing_and_recovery_is_atomic, "transactions", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  auto recovery = Recovery::open(dir.path(), io);
  CHECK(recovery);
  auto hot = std::make_shared<MemoryHotStore>();
  auto disk = std::make_shared<FailingDiskStore>();
  EngineConfig config;
  config.database_dir = dir.path();
  config.file_io = io;
  config.checkpoint_on_destroy = false;
  GraphEngine engine(hot, disk, config, nullptr, recovery.value);
  CHECK(engine.recover().ok);
  io->fail_next = "sync";
  QueryContext ctx;
  TransactionBatch batch{new_transaction_id(),
                         {UpsertNode{"N", "a", {}}, UpsertNode{"N", "b", {}}}};
  auto committed = engine.commit_batch(batch, ctx);
  CHECK(!committed);
  CHECK(committed.status.code == ErrorCode::commitOutcomeUnknown);
  CHECK(!engine.get_node("a") && !engine.get_node("b"));
  CHECK(!engine.checkpoint().ok);
  io->power_loss();
  auto reopened = Recovery::open(dir.path(), io);
  CHECK(reopened);
  auto target = std::make_shared<MemoryHotStore>();
  GraphEngine after(target, disk, config, nullptr, reopened.value);
  CHECK(after.recover().ok);
  CHECK(!after.get_node("a") && !after.get_node("b"));
}
NOVA_TEST(committed_batch_replays_without_close_or_checkpoint, "transactions", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  EngineConfig config;
  config.database_dir = dir.path();
  config.file_io = io;
  config.checkpoint_on_destroy = false;
  auto disk = std::make_shared<FailingDiskStore>();
  {
    auto root = Recovery::open(dir.path(), io);
    CHECK(root);
    auto hot = std::make_shared<MemoryHotStore>();
    GraphEngine engine(hot, disk, config, nullptr, root.value);
    CHECK(engine.recover().ok);
    QueryContext ctx;
    auto result =
        engine.commit_batch({new_transaction_id(),
                             {CreateIndex{"N", "v"}, UpsertNode{"N", "a", {{"v", int64_t(7)}}},
                              UpsertNode{"N", "b", {}}, UpsertEdge{"E", "a", "b", {}}}},
                            ctx);
    CHECK(result);
    CHECK(result.value.committed_lsn == 6);
  }
  io->power_loss();
  auto root = Recovery::open(dir.path(), io);
  CHECK(root);
  auto hot = std::make_shared<MemoryHotStore>();
  GraphEngine after(hot, disk, config, nullptr, root.value);
  CHECK(after.recover().ok);
  CHECK(after.get_node("a") && after.get_node("b") && after.get_edge("a", "E", "b"));
  CHECK(after.find_nodes("N", "v", int64_t(7), 10).value.size() == 1);
}
class AllocationFailure : public ModelFileIO {
public:
  bool armed = false;
  void fault_point(std::string_view point) override {
    if (armed && point == "transaction.before_stage")
      throw std::bad_alloc();
  }
};
NOVA_TEST(allocation_failure_precedes_wal_and_preserves_live_state, "transactions", "") {
  TempDirectory dir;
  auto io = std::make_shared<AllocationFailure>();
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
  auto before = io->read(dir.path() / root.value->root().active.filename, 1024).value;
  io->armed = true;
  CHECK(engine.upsert_node("N", "a", {}).code == ErrorCode::limitExceeded);
  CHECK(engine.execute_dsl("upsert node N b").status.code == ErrorCode::limitExceeded);
  CHECK(!engine.get_node("a") && !engine.get_node("b"));
  CHECK(io->read(dir.path() / root.value->root().active.filename, 1024).value == before);
}
class ThrowAfterWalSync : public ModelFileIO {
public:
  bool armed = false;
  Status sync_file(const std::filesystem::path &path) override {
    auto result = ModelFileIO::sync_file(path);
    if (armed && path.filename().string().starts_with("wal-") && result.ok) {
      armed = false;
      throw std::bad_alloc();
    }
    return result;
  }
};
NOVA_TEST(exception_after_commit_sync_is_unknown_and_fenced, "transactions", "") {
  TempDirectory dir;
  auto io = std::make_shared<ThrowAfterWalSync>();
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
  io->armed = true;
  auto result = engine.upsert_node("N", "a", {});
  CHECK(result.code == ErrorCode::commitOutcomeUnknown);
  CHECK(!engine.get_node("a"));
  CHECK(engine.upsert_node("N", "b", {}).code == ErrorCode::closed);
  io->power_loss();
  auto reopened = Recovery::open(dir.path(), io);
  CHECK(reopened);
  auto other = std::make_shared<MemoryHotStore>();
  GraphEngine recovered(other, disk, config, nullptr, reopened.value);
  CHECK(recovered.recover().ok);
  CHECK(recovered.get_node("a"));
  CHECK(!recovered.get_node("b"));
}

class CancelAtCommit : public ModelFileIO {
public:
  QueryContext *context=nullptr;std::string point;bool fail_sync=false;
  void fault_point(std::string_view name) override {
    if(context && name==point) {context->cancel();if(fail_sync)fail_next="sync";}
  }
};
NOVA_TEST(cancellation_preserves_commit_receipt_and_unknown_outcome, "transactions", "") {
  for(auto point : {"transaction.before_stage","wal.after_bytes","wal.after_commit","wal.after_sync"})
  for(bool fail : {false,true}) {
    TempDirectory dir;auto io=std::make_shared<CancelAtCommit>();
    auto root=Recovery::open(dir.path(),io);CHECK(root);
    EngineConfig config;config.database_dir=dir.path();config.file_io=io;config.checkpoint_on_destroy=false;
    auto hot=std::make_shared<MemoryHotStore>();auto disk=std::make_shared<FailingDiskStore>();
    GraphEngine engine(hot,disk,config,nullptr,root.value);CHECK(engine.recover().ok);
    QueryContext context;io->context=&context;io->point=point;io->fail_sync=fail;
    const auto result=engine.execute_dsl("upsert node N a",{},context);
    io->context=nullptr;io->fail_next.clear();
    if(io->point=="transaction.before_stage") {
      CHECK(result.status.code==ErrorCode::cancelled);CHECK(!engine.get_node("a"));
    } else if(fail && io->point!="wal.after_sync") {
      CHECK(result.status.code==ErrorCode::commitOutcomeUnknown);
      CHECK(!result.status.context.transaction_id.empty());CHECK(!engine.get_node("a"));
    } else {CHECK(result.ok);CHECK(result.receipt);CHECK(engine.get_node("a"));}
  }
}

// An unrelated large payload must not be copied into every small transaction.
NOVA_TEST(small_transaction_on_large_graph_fits_bounded_workspace, "transactions", "") {
  EngineFixture f;
  std::vector<StoredRecord> records;
  for (int i=0;i<10000;++i) {
    GraphObject object;object.kind=ObjectKind::Node;object.id="large"+std::to_string(i);
    object.label_or_type="N";object.version=1;
    object.properties={{"payload",std::string(1024,'x')},{"ordinal",int64_t(i)}};
    StoredRecord record;record.object=std::move(object);records.push_back(std::move(record));
  }
  f.hot->load_records(std::move(records),{});
  auto before=f.hot->clone();
  QueryOptions options;options.working_bytes=256*1024;
  QueryContext context(options);
  auto committed=f.engine->commit_batch({new_transaction_id(),{UpsertNode{"N","new",{{"v",int64_t(7)}}}}},context);
  CHECK(committed);
  CHECK(f.engine->get_node("new"));
  CHECK(!before->get("n/new"));
  CHECK(before->get("n/large0")->object.properties.at("payload")==Property(std::string(1024,'x')));
  CHECK(f.engine->execute_dsl("upsert node N second set v=8; get node second").ok);
  CHECK(!f.engine->execute_dsl("upsert node N ghost; delete node missing").ok);
  CHECK(!f.engine->get_node("ghost"));
}

NOVA_TEST(staged_index_budget_failure_preserves_wal_and_live_roots, "transactions", "") {
  TempDirectory dir;auto io=std::make_shared<ModelFileIO>();auto root=Recovery::open(dir.path(),io);CHECK(root);
  EngineConfig config;config.database_dir=dir.path();config.file_io=io;config.checkpoint_on_destroy=false;
  auto hot=std::make_shared<MemoryHotStore>();auto disk=std::make_shared<FailingDiskStore>();
  GraphEngine engine(hot,disk,config,nullptr,root.value);CHECK(engine.recover().ok);
  for(int i=0;i<200;++i)CHECK(engine.upsert_node("N",std::to_string(i),{{"v",int64_t(i)}}).ok);
  auto snapshot=hot->clone();auto indexes=hot->dump_indexes();
  auto wal=dir.path()/root.value->root().active.filename;
  auto before=io->read(wal,1024*1024);CHECK(before);
  QueryOptions options;options.working_bytes=32*1024;QueryContext context(options);
  auto result=engine.commit_batch({new_transaction_id(),{CreateIndex{"N","v"}}},context);
  CHECK(!result && result.status.code==ErrorCode::limitExceeded);
  CHECK(!hot->has_node_property_index("N","v"));CHECK(hot->dump_indexes()==indexes);
  CHECK(snapshot->dump_indexes()==indexes);CHECK(io->read(wal,1024*1024).value==before.value);
}

namespace {
// Real staging, indexes and WAL; the file model lets rejection checks compare
// durable bytes and simulate recovery without changing the engine's limits.
struct AdmissionFixture {
  TempDirectory dir;
  std::shared_ptr<ModelFileIO> io = std::make_shared<ModelFileIO>();
  std::shared_ptr<MemoryHotStore> hot = std::make_shared<MemoryHotStore>();
  std::shared_ptr<FailingDiskStore> disk = std::make_shared<FailingDiskStore>();
  std::shared_ptr<Recovery> recovery;
  EngineConfig config;
  std::unique_ptr<GraphEngine> engine;
  std::filesystem::path wal;
  AdmissionFixture() {
    auto opened = Recovery::open(dir.path(), io); CHECK(opened);
    recovery = opened.value;
    config.database_dir = dir.path(); config.file_io = io;
    config.checkpoint_on_destroy = false;
    engine = std::make_unique<GraphEngine>(hot, disk, config, nullptr, recovery);
    CHECK(engine->recover().ok);
    CHECK(engine->upsert_node("N", "a", {{"v", int64_t(1)}}).ok);
    CHECK(engine->upsert_node("N", "b", {}).ok);
    CHECK(engine->upsert_edge("E", "a", "b", {}).ok);
    CHECK(engine->create_node_property_index("N", "v").ok);
    wal = dir.path() / recovery->root().active.filename;
    // Place the existing state exactly at its persistent-metadata ceiling.
    config.memory.metadata_bytes = hot->memory_usage().metadata_bytes;
    engine = std::make_unique<GraphEngine>(hot, disk, config, nullptr, recovery);
  }
  std::vector<uint8_t> wal_bytes() {
    auto bytes = io->read(wal, 8*1024*1024); CHECK(bytes); return bytes.value;
  }
  Status run(bool ngql, std::vector<Mutation> mutations, const std::string &query,
             QueryContext &context) {
    if (!ngql) return engine->commit_batch({new_transaction_id(), std::move(mutations)}, context).status;
    auto result = engine->execute_dsl(query, {}, context);
    return result.ok ? Status::OK() : result.status;
  }
};
}

NOVA_TEST(transaction_overwrite_at_metadata_ceiling_commits_and_recovers, "transactions", "") {
  for (bool ngql : {false, true}) {
    AdmissionFixture f; auto retained = f.hot->clone(); auto before = f.wal_bytes();
    QueryContext context;
    CHECK(f.run(ngql, {UpsertNode{"N", "a", {{"v", int64_t(2)}}}},
                "upsert node N a set v=2; get node a", context).ok);
    CHECK(f.hot->get("n/a")->object.properties.at("v") == Property(int64_t(2)));
    CHECK(retained->get("n/a")->object.properties.at("v") == Property(int64_t(1)));
    CHECK(f.hot->memory_usage().metadata_bytes <= f.config.memory.metadata_bytes);
    CHECK(f.wal_bytes() != before);
    CHECK(f.engine->find_nodes("N", "v", int64_t(2), 10).value.size() == 1);
    CHECK(f.engine->find_nodes("N", "v", int64_t(1), 10).value.empty());
    f.engine.reset(); f.recovery.reset(); f.io->power_loss();
    auto recovered = Recovery::open(f.dir.path(), f.io); CHECK(recovered);
    auto target = std::make_shared<MemoryHotStore>();
    GraphEngine reopened(target, f.disk, f.config, nullptr, recovered.value);
    CHECK(reopened.recover().ok);
    CHECK(reopened.get_node("a").value.properties.at("v") == Property(int64_t(2)));
  }
}

NOVA_TEST(transaction_delete_at_metadata_ceiling_reclaims_state, "transactions", "") {
  for (bool ngql : {false, true}) {
    AdmissionFixture f; auto retained = f.hot->clone(); QueryContext context;
    CHECK(f.run(ngql, {DeleteNode{"a"}}, "delete node a", context).ok);
    CHECK(!f.hot->get("n/a")); CHECK(!f.engine->get_edge("a", "E", "b"));
    CHECK(f.hot->get("n/b")); CHECK(retained->get("n/a"));
    CHECK(f.hot->memory_usage().metadata_bytes < f.config.memory.metadata_bytes);
  }
}

NOVA_TEST(transaction_temporary_metadata_growth_uses_workspace, "transactions", "") {
  for (bool ngql : {false, true}) {
    AdmissionFixture f; QueryContext context;
    CHECK(f.run(ngql, {UpsertNode{"Scratch", "temp", {}}, DeleteNode{"temp"}},
                "upsert node Scratch temp; delete node temp", context).ok);
    CHECK(!f.hot->get("n/temp")); CHECK(f.hot->get("n/a"));
    CHECK(f.hot->memory_usage().metadata_bytes <= f.config.memory.metadata_bytes);
  }
}

NOVA_TEST(transaction_final_metadata_overflow_preserves_state_and_wal, "transactions", "") {
  for (bool ngql : {false, true}) {
    AdmissionFixture f; auto before = f.wal_bytes(); auto indexes = f.hot->dump_indexes();
    QueryContext context;
    auto result = f.run(ngql, {UpsertNode{"N", "a", {{"v", int64_t(2)}}},
                              UpsertNode{"N", "extra", {}}},
                       "upsert node N a set v=2; upsert node N extra", context);
    CHECK(!result.ok && result.code == ErrorCode::limitExceeded);
    CHECK(!f.hot->get("n/extra"));
    CHECK(f.hot->get("n/a")->object.properties.at("v") == Property(int64_t(1)));
    CHECK(f.hot->dump_indexes() == indexes); CHECK(f.wal_bytes() == before);
  }
}

NOVA_TEST(transaction_workspace_overflow_preserves_state_and_wal_near_metadata_limit, "transactions", "") {
  for (bool ngql : {false, true}) {
    AdmissionFixture f; auto before = f.wal_bytes(); auto indexes = f.hot->dump_indexes();
    QueryOptions options; options.working_bytes = 32*1024; QueryContext context(options);
    std::string payload(8192, 'x');
    const std::vector<Mutation> mutations{UpsertNode{"N", "a", {{"v", int64_t(1)}, {"payload", payload}}}};
    const auto query = "upsert node N a set v=1, payload=\"" + payload + "\"";
    auto result = f.run(ngql, mutations, query, context);
    CHECK(!result.ok && result.code == ErrorCode::limitExceeded);
    CHECK(f.hot->get("n/a")->object.properties.at("v") == Property(int64_t(1)));
    CHECK(f.hot->dump_indexes() == indexes); CHECK(f.wal_bytes() == before);
    // Only the request workspace changes: the same unindexed payload fits the
    // persistent metadata ceiling and commits with the default workspace.
    QueryContext enough;
    CHECK(f.run(ngql, mutations, query, enough).ok);
    CHECK(f.hot->get("n/a")->object.properties.at("payload") == Property(payload));
    CHECK(f.hot->memory_usage().metadata_bytes <= f.config.memory.metadata_bytes);
    CHECK(f.wal_bytes() != before);
  }
}
