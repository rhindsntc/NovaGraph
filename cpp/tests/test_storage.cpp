#include "TestSupport.hpp"
#include <thread>
using namespace graphdb;
using namespace nova_test;
NOVA_TEST(deletion_and_inbound_walk, "storage", "") {
  EngineFixture f;
  auto &e = *f.engine;
  auto result =
      e.execute_dsl("upsert node Person u1; upsert node Person u2; upsert edge FOLLOWS u1 -> u2;");
  CHECK(result.ok);
  auto walk = e.walk_in("u2", "FOLLOWS", 1, 10);
  CHECK(walk);
  CHECK(walk.value.size() == 1);
  CHECK(walk.value[0].id == "u1");
  auto removed = e.delete_edge("u1", "FOLLOWS", "u2");
  CHECK(removed.ok);
  walk = e.walk_in("u2", "FOLLOWS", 1, 10);
  CHECK(walk);
  CHECK(walk.value.empty());
  auto edge = e.upsert_edge("FOLLOWS", "u1", "u2", {});
  CHECK(edge.ok);
  auto node = e.delete_node("u1");
  CHECK(node.ok);
  CHECK(!e.get_node("u1"));
  CHECK(!e.get_edge("u1", "FOLLOWS", "u2"));
}
NOVA_TEST(graceful_close_persists_hot_records, "storage", "") {
  TempDirectory dir;
  {
    Handle db(dir.path());
    auto result = db.query("upsert node Person u1 set name=\"Ada\"");
    CHECK(result.find("\"ok\":true") != std::string::npos);
  }
  // Closing checkpoints: this is not a crash/WAL-only replay test.
  Handle reopened(dir.path());
  auto result = reopened.query("get node u1");
  CHECK(result.find("\"name\":\"Ada\"") != std::string::npos);
}
NOVA_TEST(cold_transition_rejects_stale_version, "storage", "") {
  EngineFixture f;
  auto status = f.engine->upsert_node("Person", "a", {});
  CHECK(status.ok);
  auto original = f.hot->get("n/a");
  CHECK(original);
  ManualClock clock{original->activity_tick};
  clock.advance(100);
  auto touched = f.hot->touch_read("n/a", original->object.last_read_ms + 100, clock.now);
  CHECK(touched.ok);
  CHECK(!f.hot->move_to_cold_if_version("n/a", original->object.version, "stale.rec", 0, 0,
                                        original->activity_version));
  CHECK(f.hot->cold_candidates(clock.now - 1, 10).empty());
  CHECK(f.hot->cold_candidates(clock.now, 10).size() == 1);
}
NOVA_TEST(failed_cold_write_retains_hot_record, "storage", "") {
  TempDirectory dir;
  auto hot = std::make_shared<MemoryHotStore>();
  auto disk = std::make_shared<FailingDiskStore>();
  EngineConfig cfg;
  cfg.database_dir = dir.path();
  cfg.hot_ttl_ms = -1;
  GraphEngine engine(hot, disk, cfg);
  auto status = engine.upsert_node("Person", "a", {{"name", std::string("Ada")}});
  CHECK(status.ok);
  CHECK(engine.sweep_once() == 0);
  CHECK(hot->hot_count() == 1);
  auto node = engine.get_node("a");
  CHECK(node);
  CHECK(std::get<std::string>(node.value.properties.at("name")) == "Ada");
}
NOVA_TEST(barrier_controls_storage_interleaving, "storage", "") {
  EngineFixture f;
  auto status = f.engine->upsert_node("Person", "a", {});
  CHECK(status.ok);
  auto original = f.hot->get("n/a");
  CHECK(original);
  OperationBarrier barrier;
  std::exception_ptr background;
  std::thread reader([&] {
    try {
      barrier.arrive_and_wait();
      f.hot->touch_read("n/a", original->object.last_read_ms + 1);
    } catch (...) {
      background = std::current_exception();
    }
  });
  try {
    barrier.wait_until_reached();
  } catch (...) {
    barrier.release();
    reader.join();
    throw;
  }
  barrier.release();
  reader.join();
  if (background)
    std::rethrow_exception(background);
  CHECK(!f.hot->move_to_cold_if_version("n/a", original->object.version, "stale.rec", 0, 0,
                                        original->activity_version));
}
NOVA_TEST(cold_traversal_survives_reopen, "storage", "") {
  for (bool inbound : {false, true}) {
    EngineFixture f;
    auto created =
        f.engine->execute_dsl("upsert node Person a set name=\"Ada\"; upsert node Person g set "
                              "name=\"Grace\"; upsert edge FOLLOWS a -> g;");
    CHECK(created.ok);
    // Start each direction with cold nodes AND edge; promotion by the first
    // traversal must not hide a recovery defect in the second direction.
    for (const auto &record : f.hot->dump_records())
      f.cold(object_key(record.object.kind, record.object.id));
    CHECK(f.hot->cold_count() == 3);
    auto checkpoint = f.engine->checkpoint();
    CHECK(checkpoint.ok);
    f.engine.reset();
    auto hot = std::make_shared<MemoryHotStore>();
    GraphEngine reopened(hot, f.disk, f.config);
    auto recovered = reopened.recover();
    CHECK(recovered.ok);
    CHECK(hot->cold_count() == 3);
    auto walk =
        reopened.execute_dsl(inbound ? "walk from g over FOLLOWS direction in depth 1 limit 10"
                                     : "walk from a over FOLLOWS depth 1 limit 10");
    CHECK(walk.ok);
    CHECK(walk.json.find(inbound ? "\"name\":\"Ada\"" : "\"name\":\"Grace\"") != std::string::npos);
  }
}

#include "graphdb/PersistentMap.hpp"
#include <map>
#include <random>
NOVA_TEST(persistent_staging_matches_ordered_model_and_keeps_old_roots, "storage", "") {
  PersistentMap<int> values;std::map<std::string,int> expected;
  std::mt19937 random(42);
  QueryContext context;
  for(int step=0;step<2000;++step) {
    auto key=std::to_string(random()%300);auto before=values;auto model=expected;
    if(random()%3) {values.set(key,step,&context);expected[key]=step;}
    else {values.erase(key,&context);expected.erase(key);}
    auto check=[](const auto &actual,const auto &want) {
      CHECK(actual.size()==want.size());auto it=actual.begin();
      for(const auto &[k,v]:want){CHECK(it!=actual.end());CHECK(it->first==k && it->second==v);++it;}
      CHECK(it==actual.end());
    };
    check(values,expected);check(before,model);
    auto it=values.upper_bound(key);auto want=expected.upper_bound(key);
    CHECK((it==values.end())==(want==expected.end()));
    if(want!=expected.end())CHECK(it->first==want->first);
  }
  auto before=values;QueryOptions tiny;tiny.working_bytes=1;QueryContext limited(tiny);
  bool rejected=false;
  try{values.set("rejected",1,&limited);}catch(const QueryFailure &e){rejected=e.status.code==ErrorCode::limitExceeded;}
  CHECK(rejected && !values.count("rejected") && values.size()==before.size());
}
NOVA_TEST(staging_reservations_release_scratch_and_outlive_request, "storage", "") {
  QueryOptions options;options.working_bytes=4096;QueryContext context(options);
  for(int i=0;i<10000;++i){auto held=context.hold_work(4096);}
  PersistentMap<int> survivor;
  {QueryContext temporary;survivor.set("retained",7,&temporary);}
  CHECK(survivor.at("retained")==7);survivor.clear();
  auto all=context.hold_work(4096);bool rejected=false;
  try{auto extra=context.hold_work(1);}catch(const QueryFailure &){rejected=true;}
  CHECK(rejected);
}
