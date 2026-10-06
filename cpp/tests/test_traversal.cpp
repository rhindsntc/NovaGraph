#include "TestSupport.hpp"
#include <fstream>
#include "../third_party/nlohmann/json.hpp"
using namespace graphdb;
using namespace nova_test;
using Json = nlohmann::json;

namespace {
void diamond(GraphEngine &engine) {
  CHECK(engine.execute_dsl("upsert node N d; upsert node N c; upsert node N b; upsert node N a; "
      "upsert edge E a -> c set weight=2; upsert edge E c -> d; upsert edge E a -> b set weight=1; "
      "upsert edge E b -> d; upsert edge E a -> a; upsert edge E d -> a; upsert edge F a -> b").ok);
}
Json query(GraphEngine &engine, const std::string &text) {
  auto result = engine.execute_dsl(text);
  if (!result.ok) throw Failure(result.error);
  return Json::parse(result.json);
}
std::vector<std::string> ids(const Json &objects) {
  std::vector<std::string> result;
  for (const auto &object : objects) result.push_back(object.at("id"));
  return result;
}
void connected(const Json &paths, bool inbound) {
  for (const auto &path : paths) {
    const auto &nodes=path.at("nodes"), &edges=path.at("edges");
    CHECK(nodes.size()==edges.size()+1);
    for (size_t i=0;i<edges.size();++i) {
      CHECK(edges[i].at(inbound ? "to" : "from")==nodes[i].at("id"));
      CHECK(edges[i].at(inbound ? "from" : "to")==nodes[i+1].at("id"));
    }
  }
}
}

NOVA_TEST(traversal_paths_choose_shortest_routes_and_actual_edges, "traversal", "") {
  EngineFixture f; diamond(*f.engine);
  auto result=query(*f.engine,"walk from a depth 3 paths");
  CHECK(ids(result.at("nodes"))==std::vector<std::string>({"b","c","d"}));
  CHECK(result.at("paths").size()==3); connected(result.at("paths"),false);
  CHECK(ids(result["paths"][2]["nodes"])==std::vector<std::string>({"a","b","d"}));
  CHECK(result["paths"][0]["edges"][0]["type"]=="E");
  CHECK(result["paths"][0]["edges"][0]["properties"]["weight"]==1);
  CHECK(query(*f.engine,"walk from a depth 3 return {id}")==Json::parse(R"([{"id":"b"},{"id":"c"},{"id":"d"}])"));
}
NOVA_TEST(traversal_inbound_paths_keep_stored_edge_orientation, "traversal", "") {
  EngineFixture f; diamond(*f.engine);
  auto result=query(*f.engine,"walk from d over E direction in depth 3 paths");
  CHECK(ids(result["nodes"])==std::vector<std::string>({"b","c","a"}));
  connected(result["paths"],true);
  CHECK(ids(result["paths"][2]["nodes"])==std::vector<std::string>({"d","b","a"}));
  CHECK(result["paths"][2]["edges"][1]["properties"]["weight"]==1);
}
NOVA_TEST(traversal_limit_counts_unique_nodes_after_cycles_and_duplicate_edges, "traversal", "") {
  EngineFixture f; diamond(*f.engine);
  auto result=query(*f.engine,"walk from a depth 3 limit 2 paths");
  CHECK(ids(result["nodes"])==std::vector<std::string>({"b","c"}));
  CHECK(result["paths"].size()==2);
  for (auto text : {"walk from a depth 0 paths", "walk from a limit 0 paths", "walk from absent paths"})
    CHECK(query(*f.engine,text)==Json::parse(R"({"nodes":[],"paths":[]})"));
}
NOVA_TEST(traversal_skips_dangling_endpoints_in_permissive_graphs, "traversal", "") {
  EngineFixture f; f.config.legacy_permissive_endpoints=true;
  f.engine=std::make_unique<GraphEngine>(f.hot,f.disk,f.config);
  CHECK(f.engine->execute_dsl("upsert node N a; upsert node N c; upsert node N d; "
      "upsert edge E a -> missing; upsert edge E missing -> c; upsert edge E a -> d").ok);
  auto legacy=f.engine->walk_out("a","E",3,10); CHECK(legacy);
  CHECK(legacy.value.size()==1); CHECK(legacy.value[0].id=="d");
  auto result=query(*f.engine,"walk from a depth 3 paths");
  CHECK(ids(result["nodes"])==std::vector<std::string>({"d"})); connected(result["paths"],false);
}
NOVA_TEST(traversal_paths_honor_output_work_and_edge_budgets_without_partial_data, "traversal", "") {
  EngineFixture f; diamond(*f.engine);
  QueryOptions options; options.result_bytes=400; QueryContext output(options);
  auto result=f.engine->execute_dsl("upsert node N sentinel; walk from a depth 3 paths",{},output);
  CHECK(!result.ok); CHECK(result.status.code==ErrorCode::limitExceeded); CHECK(result.json.empty());
  CHECK(!f.engine->get_node("sentinel"));
  options=QueryOptions{}; options.max_expanded_edges=2; QueryContext edges(options);
  result=f.engine->execute_dsl("walk from a depth 3 paths",{},edges);
  CHECK(!result.ok); CHECK(result.status.code==ErrorCode::limitExceeded); CHECK(result.json.empty());
  options=QueryOptions{}; options.working_bytes=8192; QueryContext memory(options);
  result=f.engine->execute_dsl("walk from a depth 3 paths",{},memory);
  CHECK(!result.ok); CHECK(result.status.code==ErrorCode::limitExceeded); CHECK(result.json.empty());
}
NOVA_TEST(traversal_paths_survive_cold_rebuild_and_reopen, "traversal", "") {
  EngineFixture f; diamond(*f.engine);
  const std::string text="walk from a depth 3 paths return {id,from,to,type,weight}";
  auto expected=query(*f.engine,text);
  for (const auto &record:f.hot->dump_records()) f.cold(object_key(record.object.kind,record.object.id));
  CHECK(f.engine->rebuild_indexes().ok); CHECK(query(*f.engine,text)==expected);
  QueryContext trim; CHECK(f.engine->trim_memory(0,trim)); CHECK(f.engine->checkpoint().ok);
  f.engine.reset(); f.hot=std::make_shared<MemoryHotStore>();
  f.engine=std::make_unique<GraphEngine>(f.hot,f.disk,f.config); CHECK(f.engine->recover().ok);
  CHECK(query(*f.engine,text)==expected);
  CHECK(f.engine->rebuild_indexes().ok); CHECK(query(*f.engine,text)==expected);
}
NOVA_TEST(traversal_explain_describes_paths_without_changing_projection_grammar, "traversal", "") {
  EngineFixture f;
  auto plan=query(*f.engine,"explain walk from absent direction in depth 2 paths");
  CHECK(plan["access_path"]=="InboundAdjacencyIndex"); CHECK(plan["algorithm"]=="BreadthFirst");
  CHECK(plan["result_mode"]=="paths"); CHECK(plan["tie_break"]=="canonical_edge_key");
  CHECK(f.engine->execute_dsl("upsert node N a; upsert node N b set paths='property'; upsert edge E a -> b").ok);
  CHECK(query(*f.engine,"walk from a return paths")==Json::parse(R"([{"paths":"property"}])"));
  CHECK(query(*f.engine,"walk from a paths return {id}")["paths"][0]["nodes"]==Json::parse(R"([{"id":"a"},{"id":"b"}])"));
  auto bad=f.engine->execute_dsl("walk from a paths paths"); CHECK(!bad.ok); CHECK(bad.status.code==ErrorCode::parseError);
}

NOVA_TEST(traversal_native_results_bound_paths_and_can_omit_them, "traversal", "") {
  EngineFixture f; diamond(*f.engine);
  auto nodes=f.engine->traverse_out("a","E",3,10); CHECK(nodes); CHECK(nodes.value.paths.empty());
  auto paths=f.engine->traverse_in("d","E",3,10,true); CHECK(paths);
  CHECK(paths.value.paths[2].nodes[0].id=="d"); CHECK(paths.value.paths[2].nodes[2].id=="a");
  CHECK(paths.value.paths[2].edges[1].from=="a"); CHECK(paths.value.paths[2].edges[1].to=="b");
  auto config=f.config; config.query_options.result_bytes=1000; config.checkpoint_on_destroy=false;
  GraphEngine limited(f.hot,f.disk,config);
  auto failed=limited.traverse_out("a","E",3,10,true);
  CHECK(!failed); CHECK(failed.status.code==ErrorCode::limitExceeded);
  CHECK(failed.value.nodes.empty()); CHECK(failed.value.paths.empty());
}
NOVA_TEST(traversal_canonical_order_is_independent_of_insertion_and_shortest_path_wins, "traversal", "") {
  EngineFixture first,second;
  const char *forward="upsert node N start; upsert node N z; upsert node N aa; upsert node N end; "
      "upsert edge E start -> aa; upsert edge E start -> z; upsert edge E z -> end; upsert edge Z start -> end";
  const char *reverse="upsert node N end; upsert node N aa; upsert node N z; upsert node N start; "
      "upsert edge Z start -> end; upsert edge E z -> end; upsert edge E start -> z; upsert edge E start -> aa";
  CHECK(first.engine->execute_dsl(forward).ok); CHECK(second.engine->execute_dsl(reverse).ok);
  const std::string text="walk from start depth 3 paths return {id,from,to,type}";
  auto result=query(*first.engine,text); CHECK(query(*second.engine,text)==result);
  CHECK(ids(result["nodes"])==std::vector<std::string>({"z","aa","end"}));
  CHECK(ids(result["paths"][2]["nodes"])==std::vector<std::string>({"start","end"}));
  CHECK(result["paths"][2]["edges"][0]["type"]=="Z");
  CHECK(query(*first.engine,"walk from start depth 1 paths return {id,from,to,type}")==result);
}
namespace {
class CountingTraversalDisk final : public DiskStore {
  DiskStore &disk_;
public:
  std::map<std::string,size_t> reads;
  explicit CountingTraversalDisk(DiskStore &disk) : disk_(disk) {}
  Result<std::string> write(const std::string &key,const GraphObject &object) override {return disk_.write(key,object);}
  Result<GraphObject> read(const std::string &key) override {++reads[key];return disk_.read(key);}
  Status remove(const std::string &key) override {return disk_.remove(key);}
};
}
NOVA_TEST(traversal_path_reconstruction_reuses_cold_records_without_promotion, "traversal", "") {
  EngineFixture f; diamond(*f.engine);
  for (const auto &record:f.hot->dump_records()) f.cold(object_key(record.object.kind,record.object.id));
  auto before=f.hot->memory_usage();
  auto disk=std::make_shared<CountingTraversalDisk>(*f.disk);
  auto config=f.config; config.memory.hot_payload_bytes=0; config.checkpoint_on_destroy=false;
  GraphEngine engine(f.hot,disk,config);
  auto result=query(engine,"walk from a depth 3 paths"); connected(result["paths"],false);
  CHECK(disk->reads.size()==11); // Four nodes and seven actual edges, including loops/duplicates.
  for (const auto &[key,count]:disk->reads) CHECK(count==1);
  auto after=f.hot->memory_usage(); CHECK(after.hot_payload_bytes==before.hot_payload_bytes);
  CHECK(f.hot->cold_count()==11);
  auto again=query(engine,"walk from a depth 3 paths"); CHECK(ids(again["nodes"])==ids(result["nodes"]));
  for (const auto &[key,count]:disk->reads) CHECK(count==2); // Cache ends with the traversal.
}
NOVA_TEST(traversal_cold_error_keeps_typed_failure_and_no_partial_paths, "traversal", "") {
  EngineFixture f; diamond(*f.engine); f.cold("n/d");
  auto config=f.config; config.checkpoint_on_destroy=false;
  GraphEngine broken(f.hot,std::make_shared<FailingDiskStore>(),config);
  auto result=broken.execute_dsl("walk from a depth 3 paths");
  CHECK(!result.ok); CHECK(result.status.code==ErrorCode::ioFailure); CHECK(result.json.empty());
}

NOVA_TEST(traversal_documented_example_returns_real_paths_through_c, "traversal", "") {
  auto source=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
      "examples/ngql/traversal-paths.ngql";
  std::ifstream input(source); CHECK(input.good());
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  TempDirectory directory; Handle db(directory.path());
  auto response=Json::parse(db.query(text.c_str())); CHECK(response["ok"]==true);
  auto result=response["data"]["results"].back();
  CHECK(ids(result["nodes"])==std::vector<std::string>({"bob","cyra","dina"}));
  connected(result["paths"],false);
  CHECK(ids(result["paths"][2]["nodes"])==std::vector<std::string>({"ada","bob","dina"}));
  CHECK(result["paths"][2]["edges"][0]["properties"]["since"]==2020);
}
