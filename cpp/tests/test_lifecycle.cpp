#include "TestSupport.hpp"
#include "../third_party/nlohmann/json.hpp"
#include <future>
#include <cstring>
using namespace graphdb;
using namespace nova_test;
using Json=nlohmann::json;
namespace {
Json json(GraphDBString value) {return Json::parse(take(value));}
using Request=GraphDBRequest;
Request *request_create() {return graphdb_request_create(5000);}
void request_cancel(Request *r) {graphdb_request_cancel(r);}
void request_release(Request *r) {graphdb_request_release(r);}
GraphDBString execute(GraphDBHandle *db,const char *text,Request *r) {return graphdb_execute_request_v2(db,text,std::strlen(text),nullptr,0,nullptr,r);}
GraphDBString close_result(GraphDBHandle *db) {return graphdb_close_v2(db,nullptr);}
}
NOVA_TEST(request_cancel_before_start_applies_no_mutation, "lifecycle", "") {
  TempDirectory dir;Handle db(dir.path());
  auto request=request_create();request_cancel(request);
  auto result=json(execute(db.value,"upsert node N cancelled",request));request_release(request);
  CHECK(result.at("ok")==false);
  CHECK(json(graphdb_execute_query(db.value,"get node cancelled")).at("ok")==false);
}
NOVA_TEST(explicit_close_is_idempotent_rejects_work_and_releases_directory, "lifecycle", "") {
  TempDirectory dir;Handle db(dir.path());
  CHECK(json(graphdb_execute_query(db.value,"upsert node N kept")).at("ok")==true);
  CHECK(json(close_result(db.value)).at("ok")==true);
  CHECK(json(close_result(db.value)).at("ok")==true);
  CHECK(json(graphdb_execute_query(db.value,"get node kept")).at("ok")==false);
  Handle reopened(dir.path());
  CHECK(json(graphdb_execute_query(reopened.value,"get node kept")).at("ok")==true);
}

NOVA_TEST(close_failure_is_retained_and_never_retried_by_destruction, "lifecycle", "") {
  TempDirectory dir;Handle db(dir.path());
  CHECK(json(graphdb_execute_query(db.value,"upsert node N kept")).at("ok")==true);
  std::filesystem::rename(dir.path()/"CURRENT",dir.path()/"saved-current");
  std::filesystem::create_directory(dir.path()/"CURRENT");
  auto failed=json(close_result(db.value));
  CHECK(failed.at("ok")==false);
  CHECK(failed.at("error").at("code")=="ioFailure");
  CHECK(json(close_result(db.value))==failed);
  std::filesystem::remove(dir.path()/"CURRENT");
  std::filesystem::rename(dir.path()/"saved-current",dir.path()/"CURRENT");
  Handle reopened(dir.path());
  CHECK(json(graphdb_execute_query(reopened.value,"get node kept")).at("ok")==true);
}
NOVA_TEST(concurrent_close_and_owned_requests_finish_without_invalid_handles, "lifecycle", "") {
  TempDirectory dir;Handle db(dir.path());
  CHECK(json(graphdb_execute_query(db.value,"upsert node N kept")).at("ok")==true);
  std::vector<std::future<Json>> work;
  for(int i=0;i<16;++i) {
    graphdb_retain(db.value);
    work.push_back(std::async(std::launch::async,[&,i] {
      auto out=json(i%2 ? close_result(db.value) : graphdb_execute_query_v2(db.value,"get node kept",13,nullptr,0,nullptr));
      graphdb_release(db.value);return out;
    }));
  }
  for(auto &item:work) {
    const auto out=item.get();
    CHECK(out.at("ok")==true || out.at("error").at("code")=="closed" || out.at("error").at("code")=="cancelled");
  }
  CHECK(json(close_result(db.value)).at("ok")==true);
}

namespace {
using Snapshot=GraphDBSnapshot;
GraphDBString snapshot_query(Snapshot *snapshot,const char *query) {
  return graphdb_snapshot_query_v2(snapshot,query,std::strlen(query),nullptr,0);
}
void snapshot_read(GraphDBHandle *db,const std::function<void(Snapshot *)> &body) {
  struct Body {const std::function<void(Snapshot *)> &run; std::exception_ptr failure;} state{body,{}};
  auto result=json(graphdb_read_snapshot_v2(db,nullptr,nullptr,[](Snapshot *view,void *data) {
    auto &state=*static_cast<Body *>(data);
    try {state.run(view);} catch(...) {state.failure=std::current_exception();}
  },&state));
  if(state.failure)std::rethrow_exception(state.failure);
  CHECK(result.at("ok")==true);
}
GraphDBString suspend_db(GraphDBHandle *db) {return graphdb_suspend_v2(db,nullptr);}
GraphDBString resume_db(GraphDBHandle *db) {return graphdb_resume_v2(db,nullptr);}
}

NOVA_TEST(c_snapshot_rejects_writes_reentrancy_and_close, "lifecycle", "") {
  TempDirectory dir;Handle db(dir.path());
  CHECK(json(graphdb_execute_query(db.value,"upsert node N a")).at("ok")==true);
  Json read,write,reentrant,close;
  snapshot_read(db.value,[&](Snapshot *view) {
    read=json(snapshot_query(view,"get node a"));
    write=json(snapshot_query(view,"upsert node N b"));
    reentrant=json(graphdb_execute_query_v2(db.value,"get node a",10,nullptr,0,nullptr));
    close=json(close_result(db.value));
  });
  CHECK(read.at("ok")==true);
  CHECK(write.at("ok")==false);
  CHECK(reentrant.at("ok")==false);
  CHECK(close.at("ok")==false);
}
NOVA_TEST(suspend_rejects_new_work_until_resume, "lifecycle", "") {
  TempDirectory dir;Handle db(dir.path());
  CHECK(json(graphdb_execute_query(db.value,"upsert node N a")).at("ok")==true);
  CHECK(json(suspend_db(db.value)).at("ok")==true);
  CHECK(json(graphdb_execute_query(db.value,"get node a")).at("ok")==false);
  CHECK(json(resume_db(db.value)).at("ok")==true);
  CHECK(json(graphdb_execute_query(db.value,"get node a")).at("ok")==true);
}

NOVA_TEST(suspend_after_close_never_reports_success, "lifecycle", "") {
  TempDirectory dir;Handle db(dir.path());CHECK(json(close_result(db.value)).at("ok")==true);
  CHECK(json(suspend_db(db.value)).at("ok")==false);
  CHECK(json(resume_db(db.value)).at("ok")==false);
}

NOVA_TEST(queued_request_cancels_and_close_times_out_without_freeing_snapshot, "lifecycle", "") {
  TempDirectory dir;Handle db(dir.path());
  OperationBarrier held;
  auto snapshot=std::async(std::launch::async,[&] {
    return json(graphdb_read_snapshot_v2(db.value,nullptr,nullptr,[](Snapshot *,void *state) {
      static_cast<OperationBarrier *>(state)->arrive_and_wait();
    },&held));
  });
  held.wait_until_reached();
  auto request=request_create();
  auto writer=std::async(std::launch::async,[&] {return json(execute(db.value,"upsert node N absent",request));});
  request_cancel(request);
  const auto ready=writer.wait_for(std::chrono::milliseconds(250));
  auto deadline=graphdb_request_create(30);
  auto closed=json(graphdb_close_v2(db.value,deadline));graphdb_request_release(deadline);
  auto rejected=json(execute(db.value,"get node absent",nullptr));
  held.release();
  auto result=writer.get();request_release(request);
  CHECK(ready==std::future_status::ready);
  CHECK(result.at("error").at("code")=="cancelled");
  CHECK(closed.at("error").at("code")=="deadlineExceeded");
  CHECK(rejected.at("error").at("code")=="closed");
  CHECK(snapshot.get().at("error").at("code")=="cancelled");
  CHECK(json(close_result(db.value)).at("ok")==true);
  Handle reopened(dir.path());
  CHECK(json(execute(reopened.value,"get node absent",nullptr)).at("error").at("code")=="notFound");
}
NOVA_TEST(snapshot_wrong_thread_and_cumulative_budget_are_rejected, "lifecycle", "") {
  TempDirectory dir;Handle db(dir.path());
  CHECK(json(execute(db.value,"upsert node N a",nullptr)).at("ok")==true);
  snapshot_read(db.value,[&](Snapshot *view) {
    auto wrong=std::async(std::launch::async,[&] {return json(snapshot_query(view,"get node a"));});
    CHECK(wrong.get().at("error").at("code")=="conflict");
  });
  auto options=graphdb_default_query_options();options.result_bytes=512;
  struct State {Json first,second;} state;
  auto out=json(graphdb_read_snapshot_v2(db.value,&options,nullptr,[](Snapshot *view,void *raw) {
    auto &state=*static_cast<State *>(raw);
    state.first=json(snapshot_query(view,"get node a"));
    state.second=json(snapshot_query(view,"get node a"));
  },&state));
  CHECK(out.at("ok")==true);
  CHECK(state.first.at("ok")==true);
  CHECK(state.second.at("error").at("code")=="limitExceeded");
}
NOVA_TEST(request_deadline_and_invalid_timeouts, "lifecycle", "") {
  CHECK(graphdb_request_create(0)==nullptr);CHECK(graphdb_request_create(5001)==nullptr);
  TempDirectory dir;Handle db(dir.path());
  auto request=graphdb_request_create(1);
  const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
  while(!graphdb_request_is_expired(request) && std::chrono::steady_clock::now()<until)std::this_thread::yield();
  CHECK(json(execute(db.value,"upsert node N absent",request)).at("error").at("code")=="deadlineExceeded");
  graphdb_request_release(request);
}

NOVA_TEST(suspended_pressure_trim_preserves_query_rejection, "lifecycle", "") {
  TempDirectory dir;Handle db(dir.path());
  CHECK(json(execute(db.value,"upsert node N a",nullptr)).at("ok")==true);
  CHECK(json(suspend_db(db.value)).at("ok")==true);
  const auto trimmed=json(graphdb_trim_memory_v2(db.value,0,nullptr));
  CHECK(trimmed.at("ok")==true);CHECK(trimmed.at("data").at("evicted")==1);
  CHECK(json(execute(db.value,"get node a",nullptr)).at("error").at("code")=="busy");
  CHECK(json(resume_db(db.value)).at("ok")==true);
  CHECK(json(execute(db.value,"get node a",nullptr)).at("ok")==true);
}

#include "../src/CGraphDBInternal.hpp"
namespace {
class PausedCommitIO : public PosixFileIO {
public:
  const std::string point;
  const bool fail;
  std::atomic<bool> armed{false};
  OperationBarrier reached;
  PausedCommitIO(std::string name,bool failure):point(std::move(name)),fail(failure) {}
  void fault_point(std::string_view name) override {
    if(name==point && armed.exchange(false)) {
      reached.arrive_and_wait();
      if(fail)throw std::runtime_error("injected exception after durable WAL sync");
    }
  }
};
}
NOVA_TEST(close_during_commit_drains_owned_write_and_preserves_outcome, "lifecycle", "") {
  for(const auto point:{"transaction.before_stage","wal.after_bytes","wal.after_commit","wal.after_sync"})
  for(bool fail:{false,true}) {
    if(fail && std::string_view(point)!="wal.after_sync")continue;
    TempDirectory dir;auto io=std::make_shared<PausedCommitIO>(point,fail);
    auto opened=graphdb::detail::open_with_io(dir.path().c_str(),600000,30000,nullptr,io);
    CHECK(opened.handle);graphdb_string_free(opened.error);
    std::unique_ptr<GraphDBHandle,decltype(&graphdb_release)> owner(opened.handle,graphdb_release);
    auto db=owner.get();auto request=request_create();io->armed=true;
    graphdb_retain(db);
    auto writer=std::async(std::launch::async,[&] {
      auto result=json(execute(db,"upsert node N kept",request));graphdb_release(db);return result;
    });
    io->reached.wait_until_reached();
    graphdb_retain(db);
    auto closer=std::async(std::launch::async,[&] {
      auto result=json(close_result(db));graphdb_release(db);return result;
    });
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(!graphdb_request_is_cancelled(request) && std::chrono::steady_clock::now()<until)std::this_thread::yield();
    const bool cancelled=graphdb_request_is_cancelled(request);
    const bool draining=closer.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout;
    auto second=graphdb_open_result(dir.path().c_str(),600000,30000);
    if(second.handle)graphdb_close(second.handle);
    graphdb_string_free(second.error);
    // The original owner may go away while both admitted calls hold references.
    owner.reset();io->reached.release();
    const auto result=writer.get(),closed=closer.get();request_release(request);
    CHECK(cancelled);CHECK(draining);CHECK(!second.handle);
    Handle reopened(dir.path());auto read=json(execute(reopened.value,"get node kept",nullptr));
    if(std::string_view(point)=="transaction.before_stage") {
      CHECK(result.at("error").at("code")=="cancelled");CHECK(closed.at("ok")==true);
      CHECK(read.at("error").at("code")=="notFound");
    } else if(fail) {
      CHECK(result.at("error").at("code")=="commitOutcomeUnknown");
      CHECK(!result.at("error").at("context").at("transactionId").get<std::string>().empty());
      CHECK(closed.at("error").at("code")=="closed");CHECK(read.at("ok")==true);
    } else {
      CHECK(result.at("ok")==true);CHECK(result.contains("receipt"));
      CHECK(closed.at("ok")==true);CHECK(read.at("ok")==true);
    }
  }
}


NOVA_TEST(maintenance_checkpoint_wait_honors_cancellation_and_deadline, "lifecycle", "") {
  TempDirectory dir; Handle db(dir.path());
  OperationBarrier held;
  auto snapshot=std::async(std::launch::async,[&] {
    return json(graphdb_read_snapshot_v2(db.value,nullptr,nullptr,[](Snapshot *,void *state) {
      static_cast<OperationBarrier *>(state)->arrive_and_wait();
    },&held));
  });
  held.wait_until_reached();
  auto request=request_create();
  auto checkpoint=std::async(std::launch::async,[&] {return json(graphdb_checkpoint_v2(db.value,request));});
  request_cancel(request);
  auto ready=checkpoint.wait_for(std::chrono::milliseconds(250));
  auto deadline=graphdb_request_create(30);
  auto timed=std::async(std::launch::async,[&] {return json(graphdb_checkpoint_v2(db.value,deadline));});
  auto timed_ready=timed.wait_for(std::chrono::milliseconds(250));
  held.release();
  auto cancelled=checkpoint.get(),expired=timed.get();
  request_release(request); request_release(deadline);
  CHECK(snapshot.get().at("ok")==true);
  CHECK(ready==std::future_status::ready);
  CHECK(timed_ready==std::future_status::ready);
  CHECK(cancelled.at("error").at("code")=="cancelled");
  CHECK(expired.at("error").at("code")=="deadlineExceeded");
  CHECK(json(graphdb_checkpoint_v2(db.value,nullptr)).at("ok")==true);
  CHECK(json(close_result(db.value)).at("ok")==true);
  CHECK(json(graphdb_checkpoint_v2(db.value,nullptr)).at("error").at("code")=="closed");
  CHECK(json(graphdb_checkpoint_v2(nullptr,nullptr)).at("error").at("code")=="invalidArgument");
}

NOVA_TEST(maintenance_checkpoint_preserves_durable_result_after_cancellation, "lifecycle", "") {
  TempDirectory dir; auto io=std::make_shared<PausedCommitIO>("wal.after_rotation",false);
  auto opened=graphdb::detail::open_with_io(dir.path().c_str(),600000,30000,nullptr,io);
  CHECK(opened.handle); graphdb_string_free(opened.error);
  std::unique_ptr<GraphDBHandle,decltype(&graphdb_release)> owner(opened.handle,graphdb_release);
  CHECK(json(execute(owner.get(),"upsert node N kept",nullptr)).at("ok")==true);
  auto request=request_create(); io->armed=true;
  auto checkpoint=std::async(std::launch::async,[&] {return json(graphdb_checkpoint_v2(owner.get(),request));});
  io->reached.wait_until_reached();
  request_cancel(request); io->reached.release();
  auto result=checkpoint.get(); request_release(request);
  CHECK(result.at("ok")==true);
  CHECK(json(close_result(owner.get())).at("ok")==true);
  Handle reopened(dir.path());
  CHECK(json(execute(reopened.value,"get node kept",nullptr)).at("ok")==true);
}
