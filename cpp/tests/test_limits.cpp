#include "TestSupport.hpp"
#include "ObservedHotStore.hpp"
using namespace graphdb;
using namespace nova_test;

NOVA_TEST(query_workspace_tracks_overlapping_and_moved_leases, "limits", "") {
  QueryOptions options;options.working_bytes=256;QueryContext context(options);
  CHECK(context.working_used()==0);CHECK(context.working_peak()==0);
  context.reserve_work(16);
  {
    auto first=context.hold_work(64);
    {
      auto second=context.hold_work(96);
      CHECK(context.working_used()==176);CHECK(context.working_peak()==176);
      {
        auto moved=std::move(second);
        CHECK(context.working_used()==176);CHECK(context.working_peak()==176);
      }
      CHECK(context.working_used()==80);CHECK(context.working_peak()==176);
    }
    CHECK(context.working_used()==80);CHECK(context.working_peak()==176);
  }
  CHECK(context.working_used()==16);CHECK(context.working_peak()==176);
  context.reserve_work(32);
  CHECK(context.working_used()==48);CHECK(context.working_peak()==176);
  {
    auto larger=context.hold_work(160);
    CHECK(context.working_used()==208);CHECK(context.working_peak()==208);
  }
  context.reserve_work(0);
  CHECK(context.working_used()==48);CHECK(context.working_peak()==208);
  CHECK(context.working_available()==208);
}
NOVA_TEST(query_workspace_leases_survive_context_destruction, "limits", "") {
  std::optional<WorkReservation> survivor;
  {
    QueryContext context;
    survivor.emplace(context.hold_work(64));
    {
      auto temporary=context.hold_work(32);
      CHECK(context.working_used()==96);CHECK(context.working_peak()==96);
    }
    CHECK(context.working_used()==64);CHECK(context.working_peak()==96);
  }
  // Moving and releasing a published lease must not access its destroyed context.
  QueryContext independent;
  auto independent_lease=independent.hold_work(24);
  { auto moved=std::move(*survivor);survivor.reset(); }
  CHECK(independent.working_used()==24);CHECK(independent.working_peak()==24);
}
NOVA_TEST(query_workspace_rejections_preserve_current_and_peak, "limits", "") {
  QueryOptions options;options.working_bytes=192;QueryContext context(options);
  auto retained=context.hold_work(64);
  { auto temporary=context.hold_work(128); }
  bool rejected=false;
  try { auto excessive=context.hold_work(129); }
  catch(const QueryFailure &failure) { rejected=true;CHECK(failure.status.code==ErrorCode::limitExceeded); }
  CHECK(rejected);CHECK(context.working_used()==64);CHECK(context.working_peak()==192);
  context.cancel();rejected=false;
  try { context.reserve_work(1); }
  catch(const QueryFailure &failure) { rejected=true;CHECK(failure.status.code==ErrorCode::cancelled); }
  CHECK(rejected);CHECK(context.working_used()==64);CHECK(context.working_peak()==192);
  QueryContext expired(std::chrono::steady_clock::time_point::min());rejected=false;
  try { expired.reserve_work(1); }
  catch(const QueryFailure &failure) { rejected=true;CHECK(failure.status.code==ErrorCode::deadlineExceeded); }
  CHECK(rejected);CHECK(expired.working_used()==0);CHECK(expired.working_peak()==0);
}
NOVA_TEST(query_workspace_tightening_preserves_historical_peak, "limits", "") {
  QueryOptions options;options.working_bytes=256;QueryContext context(options);
  {
    auto retained=context.hold_work(64);
    { auto temporary=context.hold_work(128); }
    QueryOptions ceiling;ceiling.working_bytes=96;context.tighten(ceiling);
    CHECK(context.options().working_bytes==96);CHECK(context.working_available()==32);
    CHECK(context.working_used()==64);CHECK(context.working_peak()==192);
    bool rejected=false;ceiling.working_bytes=32;
    try { context.tighten(ceiling); }
    catch(const QueryFailure &failure) { rejected=true;CHECK(failure.status.code==ErrorCode::limitExceeded); }
    CHECK(rejected);CHECK(context.options().working_bytes==32);CHECK(context.working_available()==0);
    CHECK(context.working_used()==64);CHECK(context.working_peak()==192);
  }
  CHECK(context.working_used()==0);CHECK(context.working_peak()==192);
  context.tighten(options);
  CHECK(context.options().working_bytes==32);
  context.reserve_work(32);
  CHECK(context.working_used()==32);CHECK(context.working_peak()==192);
}
NOVA_TEST(query_defaults_reject_excessive_depth_and_results, "limits", "") {
  EngineFixture f;
  for (auto text : {"walk from missing depth 17", "find nodes Missing limit 10001"}) {
    auto result=f.engine->execute_dsl(text);
    CHECK(!result.ok); CHECK(result.status.code==ErrorCode::limitExceeded);
  }
}
NOVA_TEST(query_cpp_input_is_bounded_before_lexing, "limits", "") {
  EngineFixture f;
  auto result=f.engine->execute_dsl(std::string(1024*1024+1,' '));
  CHECK(!result.ok); CHECK(result.status.code==ErrorCode::limitExceeded);
}

NOVA_TEST(query_limits_meter_tokens_parameters_and_copied_output, "limits", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("P","a",{{"value",std::string(1024,'x')}}).ok);
  QueryOptions options; options.working_bytes=512;
  QueryContext tokens(options);
  auto lex=f.engine->execute_dsl("explain get node a; explain get node a",{},tokens);
  CHECK(!lex.ok); CHECK(lex.status.code==ErrorCode::limitExceeded);
  QueryContext params(options);
  auto bound=f.engine->execute_dsl("upsert node P b set value=$v",{{"v",std::string(2048,'x')}},params);
  CHECK(!bound.ok); CHECK(bound.status.code==ErrorCode::limitExceeded); CHECK(!f.engine->get_node("b"));
  QueryContext result(options);
  auto read=f.engine->execute_dsl("get node a",{},result);
  CHECK(!read.ok); CHECK(read.status.code==ErrorCode::limitExceeded);
}
NOVA_TEST(query_limits_apply_to_direct_engine_reads, "limits", "") {
  EngineFixture f;
  auto walk=f.engine->walk_out("missing","",17,1);
  CHECK(!walk); CHECK(walk.status.code==ErrorCode::limitExceeded);
  auto find=f.engine->find_nodes("P","",{},10001);
  CHECK(!find); CHECK(find.status.code==ErrorCode::limitExceeded);
}
NOVA_TEST(query_edge_work_budget_aborts_without_partial_results, "limits", "") {
  EngineFixture f;
  CHECK(f.engine->execute_dsl("upsert node P a; upsert node P b; upsert node P c; upsert edge E a -> b; upsert edge E a -> c").ok);
  QueryOptions options; options.max_expanded_edges=1; QueryContext context(options);
  auto result=f.engine->execute_dsl("walk from a depth 1 limit 10",{},context);
  CHECK(!result.ok); CHECK(result.status.code==ErrorCode::limitExceeded); CHECK(result.json.empty());
}
NOVA_TEST(query_limits_reject_batch_before_publication, "limits", "") {
  EngineFixture f;
  QueryOptions options; options.batch_bytes=16; QueryContext context(options);
  auto result=f.engine->execute_dsl("upsert node P a set name='cannot fit'",{},context);
  CHECK(!result.ok); CHECK(result.status.code==ErrorCode::limitExceeded); CHECK(!f.engine->get_node("a"));
}
NOVA_TEST(query_cancelled_and_expired_contexts_reject_mutations, "limits", "") {
  EngineFixture f;
  QueryContext cancelled; cancelled.cancel();
  auto result=f.engine->execute_dsl("upsert node P a",{},cancelled);
  CHECK(!result.ok); CHECK(result.status.code==ErrorCode::cancelled); CHECK(!f.engine->get_node("a"));
  QueryContext expired(std::chrono::steady_clock::time_point::min());
  result=f.engine->execute_dsl("upsert node P a",{},expired);
  CHECK(!result.ok); CHECK(result.status.code==ErrorCode::deadlineExceeded); CHECK(!f.engine->get_node("a"));
}

NOVA_TEST(query_output_budget_counts_small_empty_results_and_rolls_back_large_ones, "limits", "") {
  EngineFixture f;
  QueryOptions options; options.result_bytes=2; QueryContext empty(options);
  auto result=f.engine->execute_dsl("find nodes Missing",{},empty);
  CHECK(result.ok); CHECK(result.json=="[]");
  options.result_bytes=80; QueryContext large(options);
  result=f.engine->execute_dsl("upsert node P a set value='large'; get node a",{},large);
  CHECK(!result.ok); CHECK(result.status.code==ErrorCode::limitExceeded); CHECK(!f.engine->get_node("a"));
}
NOVA_TEST(query_config_cannot_be_bypassed_with_a_looser_request_context, "limits", "") {
  EngineFixture f; auto config=f.config; config.query_options.max_depth=1;
  GraphEngine engine(f.hot,f.disk,config); QueryContext loose;
  auto result=engine.execute_dsl("walk from missing depth 2",{},loose);
  CHECK(!result.ok); CHECK(result.status.code==ErrorCode::limitExceeded);
}

NOVA_TEST(query_c_configuration_enforces_limits_and_rejects_invalid_options, "limits", "") {
  TempDirectory dir;
  auto options=graphdb_default_query_options(); options.max_depth=1;
  auto opened=graphdb_open_with_options_result(dir.path().c_str(),600000,10,&options);
  CHECK(opened.handle); CHECK(!opened.error.data);
  auto result=take(graphdb_execute_query(opened.handle,"walk from missing depth 2"));
  graphdb_close(opened.handle);
  CHECK(result.find("\"code\":\"limitExceeded\"")!=std::string::npos);
  options.working_bytes=0;
  auto invalid=graphdb_open_with_options_result((dir.path()/"absent").c_str(),600000,10,&options);
  if(invalid.handle)graphdb_close(invalid.handle);
  CHECK(!invalid.handle);
  CHECK(take(invalid.error).find("invalidArgument")!=std::string::npos);
  CHECK(!std::filesystem::exists(dir.path()/"absent"));
}

// A lazy valid million-edge graph makes the fanout contract deterministic without
// allocating a million fixtures on every CI run; query execution itself is real.
class MillionEdgeStore final : public ObservedHotStore {
public:
  size_t edges_read=0;
  MillionEdgeStore() {
    GraphObject a; a.id="a";a.label_or_type="P"; CHECK(store.put_hot("n/a",a).ok);
    GraphObject b; b.id="b";b.label_or_type="P"; CHECK(store.put_hot("n/b",b).ok);
  }
  std::optional<std::string> next_query_key(QueryIndex kind,const std::string &id,const std::string &type,
      const Property &value,const std::string &after,QueryContext &context) override {
    if(kind!=QueryIndex::Outbound || id!="a")return store.next_query_key(kind,id,type,value,after,context);
    // All edges target b; each has a different type, so the identities are distinct.
    size_t next=after.empty()?0:std::stoull(after.substr(2))+1;
    if(next==1000000)return {};
    context.enforce();context.reserve_work(128);
    return "e/"+std::to_string(next);
  }
  size_t query_record_charge(const std::string &key) const override {return key.starts_with("e/") ? 1024:store.query_record_charge(key);}
  std::optional<StoredRecord> get(const std::string &key,bool properties=true) override {
    if(!key.starts_with("e/"))return store.get(key,properties);
    if(properties)++edges_read;
    StoredRecord record; record.object.kind=ObjectKind::Edge; record.object.from="a";record.object.to="b";
    record.object.label_or_type="E"+key.substr(2);record.object.id=edge_id("a",record.object.label_or_type,"b");record.payload_bytes=128;
    return record;
  }
};
NOVA_TEST(query_million_edge_fanout_stops_at_work_budget, "limits", "") {
  TempDirectory dir; auto hot=std::make_shared<MillionEdgeStore>();
  auto disk=std::make_shared<FileDiskStore>(dir.path()/"cold");
  EngineConfig config;config.database_dir=dir.path();config.checkpoint_on_destroy=false;
  GraphEngine engine(hot,disk,config);
  QueryOptions options;options.max_expanded_edges=3;QueryContext context(options);
  auto result=engine.execute_dsl("walk from a depth 1 limit 2",{},context);
  CHECK(!result.ok);CHECK(result.status.code==ErrorCode::limitExceeded);CHECK(result.json.empty());CHECK(hot->edges_read==3);
}
NOVA_TEST(query_real_index_cursor_uses_small_workspace_on_large_bucket, "limits", "") {
  EngineFixture f;
  for(size_t i=0;i<4000;++i) {GraphObject o;o.id=std::to_string(i);o.label_or_type="P";CHECK(f.hot->put_hot(object_key(ObjectKind::Node,o.id),o).ok);}
  auto config=f.config;config.memory.query_bytes=32768;config.query_options.working_bytes=32768;config.checkpoint_on_destroy=false;
  GraphEngine engine(f.hot,f.disk,config);
  auto result=engine.execute_dsl("find nodes P limit 1 return {id}");
  CHECK(result.ok);CHECK(result.json=="[{\"id\":\"0\"}]");
}

NOVA_TEST(query_c_parameter_transport_respects_small_working_budget, "limits", "") {
  TempDirectory dir;auto options=graphdb_default_query_options();options.working_bytes=512;
  auto opened=graphdb_open_with_options_result(dir.path().c_str(),600000,10,&options);CHECK(opened.handle);
  // Invalid JSON would produce invalidArgument if parsed before its request budget check.
  std::string parameters(2048,'!');
  auto result=take(graphdb_execute_query_with_params(opened.handle,"get node a",parameters.c_str()));
  graphdb_close(opened.handle);
  CHECK(result.find("\"code\":\"limitExceeded\"")!=std::string::npos);
}
NOVA_TEST(query_native_point_read_honors_configured_working_budget, "limits", "") {
  EngineFixture f;CHECK(f.engine->upsert_node("P","a",{{"value",std::string(1024,'x')}}).ok);
  auto config=f.config;config.query_options.working_bytes=512;config.checkpoint_on_destroy=false;
  GraphEngine engine(f.hot,f.disk,config);
  auto result=engine.get_node("a");CHECK(!result);CHECK(result.status.code==ErrorCode::limitExceeded);
}

NOVA_TEST(query_limits_apply_to_native_transaction_batches, "limits", "") {
  EngineFixture f;auto config=f.config;config.query_options.max_statements=1;config.checkpoint_on_destroy=false;
  GraphEngine engine(f.hot,f.disk,config);QueryContext context;
  TransactionBatch batch{new_transaction_id(),{UpsertNode{"P","a",{}},UpsertNode{"P","b",{}}}};
  auto result=engine.commit_batch(batch,context);CHECK(!result);CHECK(result.status.code==ErrorCode::limitExceeded);
  CHECK(!engine.get_node("a"));
}

NOVA_TEST(query_maintenance_and_index_build_honor_active_working_budget, "limits", "") {
  for(auto text:{"rebuild indexes","create index on P(value)","checkpoint"}) {
    EngineFixture f;CHECK(f.engine->upsert_node("P","a",{{"value",std::string(65536,'x')}}).ok);f.cold("n/a");
    QueryOptions options;options.working_bytes=16384;QueryContext context(options);
    auto result=f.engine->execute_dsl(text,{},context);
    CHECK(!result.ok);CHECK(result.status.code==ErrorCode::limitExceeded);
    CHECK(!f.engine->has_node_property_index("P","value"));
  }
}

NOVA_TEST(query_staging_shares_unmodified_property_maps_but_meters_reads, "limits", "") {
  for(bool native:{false,true}) {
    EngineFixture f;GraphObject object;object.id="a";object.label_or_type="P";
    for(int i=0;i<1000;++i)object.properties[std::to_string(i)]=std::monostate{};
    CHECK(f.hot->put_hot("n/a",std::move(object)).ok);
    auto config=f.config;config.query_options.working_bytes=32768;config.checkpoint_on_destroy=false;
    GraphEngine engine(f.hot,f.disk,config);
    auto status=native ? engine.upsert_node("P","b",{}) : engine.execute_dsl("upsert node P b").status;
    CHECK(status.ok);CHECK(engine.get_node("b"));
    // Unchanged properties are shared, but reading them still requires copied-value workspace.
    auto read=engine.get_node("a");CHECK(!read);CHECK(read.status.code==ErrorCode::limitExceeded);
  }
}
NOVA_TEST(query_parameter_size_cannot_wrap_platform_size_t, "limits", "") {
  EngineFixture f;
  auto result=f.engine->execute_dsl("find nodes P limit $limit",{{"limit",int64_t(4294967296LL)}});
  CHECK(!result.ok);
  CHECK(result.status.code==(sizeof(size_t)==4 ? ErrorCode::parseError : ErrorCode::limitExceeded));
}

NOVA_TEST(deep_tree_read_budget_rejects_before_activity_copy, "limits", "") {
  EngineFixture f;
  for(int i=0;i<30000;++i) {GraphObject o;o.id=std::to_string(i);o.label_or_type="N";auto key="n/"+o.id;CHECK(f.hot->put_hot(key,std::move(o)).ok);}
  CHECK(f.engine->get_node("0"));
  auto activity=f.hot->get("n/0",false)->activity_version;
  auto config=f.config;config.checkpoint_on_destroy=false;config.query_options.working_bytes=1928;
  GraphEngine limited(f.hot,f.disk,config);
  auto read=limited.get_node("0");CHECK(!read);CHECK(read.status.code==ErrorCode::limitExceeded);
  CHECK(f.hot->get("n/0",false)->activity_version==activity);
}
