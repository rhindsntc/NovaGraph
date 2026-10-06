#include "TestSupport.hpp"
#include <future>
using namespace graphdb;
using namespace nova_test;
NOVA_TEST(snapshot_blocks_writer_and_rejects_reentrant_write, "consistency", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N", "a", {{"v", int64_t(1)}}).ok);
  std::future<Status> writer;
  {
    auto snapshot = f.engine->read_snapshot();
    CHECK(snapshot.get_node("a"));
    CHECK(!f.engine->upsert_node("N", "b", {}).ok);
    OperationBarrier started;
    writer = std::async(std::launch::async, [&] {
      started.arrive_and_wait();
      return f.engine->upsert_node("N", "a", {{"v", int64_t(2)}});
    });
    started.wait_until_reached();
    started.release();
    CHECK(writer.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout);
    CHECK(std::get<int64_t>(snapshot.get_node("a").value.properties.at("v")) == 1);
  }
  CHECK(writer.get().ok);
  CHECK(std::get<int64_t>(f.engine->get_node("a").value.properties.at("v")) == 2);
}
NOVA_TEST(nested_snapshots_on_distinct_engines_still_reject_outer_write, "consistency", "") {
  EngineFixture first, second;
  auto outer = first.engine->read_snapshot();
  auto inner = second.engine->read_snapshot();
  CHECK(first.engine->upsert_node("N", "a", {}).code == ErrorCode::conflict);
  CHECK(second.engine->upsert_node("N", "b", {}).code == ErrorCode::conflict);
}

NOVA_TEST(query_cancel_and_deadline_interrupt_gate_wait, "consistency", "") {
  EngineFixture f;
  for (int operation : {0,1,2,3}) for (bool cancel : {false, true}) {
    QueryOptions options; options.timeout_ms=50;
    QueryContext context(options);
    std::future<QueryResult> reader;
    bool ready=false;
    {
      auto snapshot=f.engine->read_snapshot();
      OperationBarrier started;
      reader=std::async(std::launch::async,[&] {
        started.arrive_and_wait();
        if(operation==0)return f.engine->execute_dsl("get node absent",{},context);
        Status status;
        if(operation==1)status=f.engine->commit_batch({new_transaction_id(),{UpsertNode{"N","a",{}}}},context).status;
        if(operation==2)status=f.engine->trim_memory(0,context).status;
        if(operation==3)status=f.engine->collect_garbage(1,context).status;
        return QueryResult(status.ok,{},status.message,status);
      });
      started.wait_until_reached(); started.release();
      if(cancel)context.cancel();
      ready=reader.wait_for(std::chrono::milliseconds(250))==std::future_status::ready;
    }
    const auto result=reader.get();
    CHECK(ready);
    CHECK(result.status.code==(cancel ? ErrorCode::cancelled : ErrorCode::deadlineExceeded));
  }
}

NOVA_TEST(snapshot_dsl_reads_one_view_and_rejects_mutation, "consistency", "") {
  EngineFixture f;CHECK(f.engine->upsert_node("N","a",{{"v",int64_t(1)}}).ok);
  auto snapshot=f.engine->read_snapshot();
  auto result=snapshot.execute_dsl("get node a");
  CHECK(result.ok);
  for(const auto *query : {"upsert node N rejected", "checkpoint", "rebuild indexes", "get node a; delete node a"})
    CHECK(snapshot.execute_dsl(query).status.code==ErrorCode::conflict);
  CHECK(snapshot.get_node("a"));
}

#include "ObservedHotStore.hpp"
#include "ModelFileIO.hpp"
namespace {
class CancelCursorStore : public ObservedHotStore {
public:
  size_t visited=0;bool armed=false;
  std::optional<std::string> next_query_key(QueryIndex i,const std::string &a,const std::string &p,const Property &v,const std::string &after,QueryContext &c) override {
    auto key=ObservedHotStore::next_query_key(i,a,p,v,after,c);
    if(armed && ++visited==2)c.cancel();
    return key;
  }
};
class CancelColdIO : public ModelFileIO {
public:
  QueryContext *context=nullptr;
  Result<std::vector<uint8_t>> read(const std::filesystem::path &path,size_t maximum) override {
    auto result=ModelFileIO::read(path,maximum);
    if(context && path.filename().string().starts_with("payload-"))context->cancel();
    return result;
  }
};
}
NOVA_TEST(active_scan_and_traversal_cancel_without_partial_results, "consistency", "") {
  TempDirectory dir;auto hot=std::make_shared<CancelCursorStore>();auto disk=std::make_shared<FailingDiskStore>();
  EngineConfig config;config.database_dir=dir.path();config.checkpoint_on_destroy=false;
  GraphEngine engine(hot,disk,config);
  CHECK(engine.execute_dsl("upsert node N a; upsert node N b; upsert node N c; upsert edge E a -> b; upsert edge E a -> c").ok);
  for(auto query:{"find nodes N","walk from a depth 2 paths"}) {
    hot->visited=0;hot->armed=true;QueryContext context;
    const auto result=engine.execute_dsl(query,{},context);hot->armed=false;
    CHECK(result.status.code==ErrorCode::cancelled);CHECK(result.json.empty());CHECK(hot->visited==2);
  }
}
NOVA_TEST(cancellation_after_cold_io_prevents_promotion, "consistency", "") {
  TempDirectory dir;auto io=std::make_shared<CancelColdIO>();auto hot=std::make_shared<MemoryHotStore>();
  auto disk=std::make_shared<FileDiskStore>(dir.path()/"cold",io);
  EngineConfig config;config.database_dir=dir.path();config.file_io=io;config.checkpoint_on_destroy=false;
  GraphEngine engine(hot,disk,config);CHECK(engine.upsert_node("N","a",{}).ok);
  QueryContext trim;CHECK(engine.trim_memory(0,trim));CHECK(hot->hot_count()==0);
  QueryContext request;io->context=&request;
  const auto result=engine.execute_dsl("get node a",{},request);io->context=nullptr;
  CHECK(result.status.code==ErrorCode::cancelled);CHECK(hot->hot_count()==0);
  CHECK(engine.get_node("a"));
}

NOVA_TEST(snapshot_gate_wait_uses_database_timeout_before_acquisition, "consistency", "") {
  EngineFixture f;auto config=f.config;config.query_options.timeout_ms=30;
  GraphEngine engine(f.hot,f.disk,config);
  std::future<Status> waiter;bool ready=false;
  {
    auto held=engine.read_snapshot();
    waiter=std::async(std::launch::async,[&] {
      QueryOptions loose;loose.timeout_ms=1000;QueryContext context(loose);
      try {auto snapshot=engine.read_snapshot(context);return Status::OK();}
      catch(const QueryFailure &error) {return error.status;}
    });
    ready=waiter.wait_for(std::chrono::milliseconds(250))==std::future_status::ready;
  }
  auto status=waiter.get();CHECK(ready);CHECK(status.code==ErrorCode::deadlineExceeded);
}
