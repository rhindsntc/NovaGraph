#include "TestSupport.hpp"
using namespace graphdb;
using namespace nova_test;
NOVA_TEST(cold_indexes_rebuild_and_reopen_equivalent, "indexes", "") {
  EngineFixture f;
  CHECK(f.engine->create_node_property_index("N", "v").ok);
  CHECK(f.engine->upsert_node("N", "a", {{"v", int64_t(1)}, {"large", std::string(1000, 'x')}}).ok);
  f.cold("n/a");
  CHECK(f.engine->rebuild_indexes().ok);
  auto found = f.engine->find_nodes("N", "v", 1.0, 10);
  CHECK(found);
  CHECK(found.value.size() == 1);
  f.cold("n/a");
  CHECK(f.engine->checkpoint().ok);
  f.engine.reset();
  auto hot = std::make_shared<MemoryHotStore>();
  auto no_read = std::make_shared<FailingDiskStore>();
  GraphEngine opened(hot, no_read, f.config);
  CHECK(opened.recover().ok);
  CHECK(hot->node_keys_by_label_property("N", "v", int64_t(1), 10).size() == 1);
  CHECK(hot->cold_count() == 1);
}
NOVA_TEST(failed_rebuild_preserves_current_indexes, "indexes", "") {
  EngineFixture f;
  CHECK(f.engine->create_node_property_index("N", "v").ok);
  CHECK(f.engine->upsert_node("N", "a", {{"v", int64_t(1)}}).ok);
  f.cold("n/a");
  GraphEngine broken(f.hot, std::make_shared<FailingDiskStore>(), f.config);
  CHECK(!broken.rebuild_indexes().ok);
  CHECK(f.hot->node_keys_by_label_property("N", "v", int64_t(1), 10).size() == 1);
}
NOVA_TEST(reads_and_residency_do_not_change_logical_version, "indexes", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N", "a", {}).ok);
  auto before = f.hot->get("n/a")->object.version;
  CHECK(f.engine->get_node("a"));
  CHECK(f.hot->get("n/a")->object.version == before);
  f.cold("n/a");
  CHECK(f.hot->get("n/a")->object.version == before);
  CHECK(f.engine->get_node("a"));
  CHECK(f.hot->get("n/a")->object.version == before);
}
NOVA_TEST(cold_payload_wrong_identity_is_corruption, "indexes", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N", "a", {}).ok);
  f.cold("n/a");
  auto manifest = f.hot->get("n/a");
  GraphObject wrong;
  wrong.id = "b";
  wrong.label_or_type = "N";
  wrong.version = manifest->object.version;
  PosixFileIO io;
  CHECK(io.write_all(f.dir.path() / "cold" / manifest->disk_key, serialize_object_binary(wrong),
                     WriteMode::replace)
            .ok);
  auto read = f.engine->get_node("a");
  CHECK(!read);
  CHECK(read.status.code == ErrorCode::corruptData);
}
NOVA_TEST(indexed_scanned_and_bidirectional_results_survive_storage_states, "indexes", "") {
  EngineFixture f;
  CHECK(f.engine
            ->execute_dsl("create index on N(v); upsert node N a set v=1,u=1; upsert node N b set "
                          "v=1,u=1; upsert edge E a -> b")
            .ok);
  auto expected = f.hot->dump_indexes();
  for (auto rec : f.hot->dump_records())
    f.cold(object_key(rec.object.kind, rec.object.id));
  CHECK(f.hot->dump_indexes() == expected);
  CHECK(f.engine->checkpoint().ok);
  f.engine.reset();
  f.hot = std::make_shared<MemoryHotStore>();
  f.engine = std::make_unique<GraphEngine>(f.hot, f.disk, f.config);
  CHECK(f.engine->recover().ok);
  CHECK(f.hot->dump_indexes() == expected);
  CHECK(f.engine->rebuild_indexes().ok);
  CHECK(f.hot->dump_indexes() == expected);
  auto indexed = f.engine->find_nodes("N", "v", int64_t(1), 10);
  auto scanned = f.engine->find_nodes("N", "u", int64_t(1), 10);
  CHECK(indexed && scanned);
  CHECK(indexed.value.size() == 2 && scanned.value.size() == 2);
  auto out = f.engine->walk_out("a", "E", 1, 10), in = f.engine->walk_in("b", "E", 1, 10);
  CHECK(out && in);
  CHECK(out.value.size() == 1 && out.value[0].id == "b");
  CHECK(in.value.size() == 1 && in.value[0].id == "a");
  CHECK(f.hot->dump_indexes() == expected);
}
NOVA_TEST(declaring_index_reads_existing_cold_properties, "indexes", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N", "a", {{"v", int64_t(7)}}).ok);
  f.cold("n/a");
  CHECK(f.engine->create_node_property_index("N", "v").ok);
  CHECK(f.hot->node_keys_by_label_property("N", "v", int64_t(7), 10).size() == 1);
  CHECK(f.hot->cold_count() == 1);
}

// Index one scalar without retaining copies of every unrelated payload.
NOVA_TEST(index_large_payloads_with_bounded_workspace, "indexes", "") {
  EngineFixture f;
  std::vector<StoredRecord> records;
  for (int i=0;i<1000;++i) {
    GraphObject object;object.kind=ObjectKind::Node;object.id="n"+std::to_string(i);
    object.label_or_type="N";object.version=1;
    object.properties={{"payload",std::string(1024,'x')},{"v",int64_t(i)}};
    StoredRecord record;record.object=std::move(object);records.push_back(std::move(record));
  }
  f.hot->load_records(std::move(records),{});
  f.cold("n/n0");
  auto before=f.hot->clone();
  QueryOptions options;options.working_bytes=2*1024*1024;
  QueryContext context(options);
  CHECK(f.engine->commit_batch({new_transaction_id(),{CreateIndex{"N","v"}}},context));
  CHECK(f.hot->has_node_property_index("N","v"));
  CHECK(!before->has_node_property_index("N","v"));
  CHECK(f.hot->node_keys_by_label_property("N","v",int64_t(0),10)==std::vector<std::string>{"n/n0"});
  CHECK(f.hot->node_keys_by_label_property("N","v",int64_t(999),10)==std::vector<std::string>{"n/n999"});
  CHECK(f.hot->cold_count()==1);
}

NOVA_TEST(failed_new_cold_index_does_not_publish_partial_postings, "indexes", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N","a",{{"v",int64_t(1)}}).ok);
  CHECK(f.engine->upsert_node("N","z",{{"v",int64_t(2)}}).ok);f.cold("n/z");
  auto indexes=f.hot->dump_indexes();auto snapshot=f.hot->clone();
  auto config=f.config;config.checkpoint_on_destroy=false;
  GraphEngine broken(f.hot,std::make_shared<FailingDiskStore>(),config);
  CHECK(!broken.create_node_property_index("N","v").ok);
  CHECK(!f.hot->has_node_property_index("N","v"));
  CHECK(f.hot->dump_indexes()==indexes && snapshot->dump_indexes()==indexes);
  CHECK(f.hot->cold_count()==1);
}

NOVA_TEST(hot_scalar_index_does_not_reserve_unrelated_payload, "indexes", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N","a",{{"payload",std::string(128*1024,'x')},{"v",int64_t(7)}}).ok);
  QueryOptions options;options.working_bytes=2*1024*1024;QueryContext context(options);
  CHECK(f.engine->commit_batch({new_transaction_id(),{CreateIndex{"N","v"}}},context));
  CHECK(f.hot->node_keys_by_label_property("N","v",int64_t(7),10)==std::vector<std::string>{"n/a"});
}
NOVA_TEST(hot_label_rebuild_does_not_reserve_unrelated_payload, "indexes", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N","a",{{"payload",std::string(128*1024,'x')}}).ok);
  QueryOptions options;options.working_bytes=2*1024*1024;QueryContext context(options);
  CHECK(f.engine->execute_dsl("rebuild indexes",{},context).ok);
  CHECK(f.hot->node_keys_by_label("N",10)==std::vector<std::string>{"n/a"});
}

NOVA_TEST(compact_postings_fit_ten_thousand_keys_in_one_mib, "indexes", "") {
  QueryOptions options; options.working_bytes=1024*1024;
  QueryContext context(options);
  PersistentSet postings;
  for(int i=0;i<10000;++i) postings.insert("record:"+std::to_string(i),&context);
  CHECK(postings.size()==10000);
  std::string previous;
  for(const auto &key:postings){CHECK(previous.empty() || previous<key);previous=key;}
}

NOVA_TEST(compact_postings_match_model_across_splits_merges_and_failures, "indexes", "") {
  PersistentSet values; std::set<std::string> model;
  auto verify=[&] {
    auto actual=values.begin();
    for(const auto &key:model){CHECK(actual!=values.end());CHECK(*actual==key);++actual;}
    CHECK(actual==values.end()); CHECK(values.size()==model.size());
    for(const auto &key:model){auto a=values.upper_bound(key);auto b=model.upper_bound(key);
      CHECK((a==values.end())==(b==model.end()));if(b!=model.end())CHECK(*a==*b);}
  };
  values.insert("");model.insert("");
  values.insert(std::string("a\0b",3));model.insert(std::string("a\0b",3));
  values.insert(std::string(2048,'z'));model.insert(std::string(2048,'z'));
  for(int i=0;i<600;++i){auto key=std::to_string((i*137)%601);values.insert(key);model.insert(key);}
  verify();auto snapshot=values;auto retained=values.begin();
  for(int i=0;i<601;++i){auto key=std::to_string(i);values.erase(key);model.erase(key);verify();}
  CHECK(snapshot.size()==603); CHECK(*retained=="");
  for(size_t budget:{size_t(1),size_t(100),size_t(1024)}) {
    QueryOptions options;options.working_bytes=budget;QueryContext context(options);
    auto before=values;bool rejected=false;
    try{values.insert(std::string(4096,'x'),&context);}catch(const QueryFailure&){rejected=true;}
    CHECK(rejected);verify();
    auto released=context.hold_work(budget); // failed update released every temporary charge
  }
  values.clear();CHECK(values.empty());CHECK(*retained=="");
}
NOVA_TEST(persistent_map_floor_keeps_its_snapshot, "indexes", "") {
  PersistentMap<int> values;
  CHECK(values.floor("x")==values.end());
  values.set("b",2);values.set("d",4);values.set("f",6);
  CHECK(values.floor("a")==values.end());CHECK(values.floor("b")->second==2);
  CHECK(values.floor("e")->second==4);CHECK(values.floor("z")->second==6);
  auto old=values.floor("e");values.erase("d");CHECK(old->second==4);++old;CHECK(old->second==6);
}

NOVA_TEST(compact_posting_split_reservations_rollback_and_outlive_request, "indexes", "") {
  PersistentSet original;
  for(int i=0;i<32;++i)original.insert("key:"+std::to_string(i));
  size_t failures=0,successes=0;
  for(size_t budget=64;budget<=8192;budget+=64){
    auto candidate=original;QueryOptions options;options.working_bytes=budget;QueryContext context(options);
    try{candidate.insert("new-key",&context);++successes;CHECK(candidate.size()==33);}
    catch(const QueryFailure &e){CHECK(e.status.code==ErrorCode::limitExceeded);++failures;CHECK(candidate.size()==32 && !candidate.count("new-key"));}
    candidate.clear();CHECK(context.working_available()==budget);
  }
  CHECK(failures>1 && successes>1);
  PersistentSet retained;
  {QueryContext context;for(int i=0;i<100;++i)retained.insert(std::to_string(i),&context);}
  auto snapshot=retained.begin();retained.clear();CHECK(*snapshot=="0");
  QueryContext cancelled;cancelled.cancel();bool rejected=false;
  try{original.erase("key:0",&cancelled);}catch(const QueryFailure &e){rejected=e.status.code==ErrorCode::cancelled;}
  CHECK(rejected && original.count("key:0"));
}
