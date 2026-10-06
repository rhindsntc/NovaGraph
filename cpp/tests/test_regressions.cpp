#include "TestSupport.hpp"
#include "graphdb/DslParser.hpp"
#include <fstream>
using namespace graphdb;
using namespace nova_test;
static void indexed_cold_node(EngineFixture &f) {
  auto index = f.engine->create_node_property_index("Person", "email");
  CHECK(index.ok);
  auto node = f.engine->upsert_node("Person", "a", {{"email", std::string("a@nova.test")}});
  CHECK(node.ok);
  f.cold("n/a");
  CHECK(f.hot->cold_count() == 1);
}
NOVA_TEST(cold_index_survives_reopen, "regressions", "") {
  EngineFixture f;
  indexed_cold_node(f);
  auto checkpoint = f.engine->checkpoint();
  CHECK(checkpoint.ok);
  f.engine.reset();
  auto hot = std::make_shared<MemoryHotStore>();
  GraphEngine reopened(hot, f.disk, f.config);
  auto recovered = reopened.recover();
  CHECK(recovered.ok);
  CHECK(hot->cold_count() == 1);
  auto found = reopened.find_nodes("Person", "email", std::string("a@nova.test"), 10);
  CHECK(found);
  CONTRACT("cold_index_survives_reopen", found.value.size() == 1);
}
NOVA_TEST(cold_index_survives_rebuild, "regressions", "") {
  EngineFixture f;
  indexed_cold_node(f);
  auto rebuilt = f.engine->rebuild_indexes();
  CHECK(rebuilt.ok);
  auto found = f.engine->find_nodes("Person", "email", std::string("a@nova.test"), 10);
  CHECK(found);
  CONTRACT("cold_index_survives_rebuild", found.value.size() == 1);
}
NOVA_TEST(failed_batch_rolls_back, "regressions", "") {
  EngineFixture f;
  auto batch = f.engine->execute_dsl("upsert node Person a; delete node missing;");
  CHECK(!batch.ok);
  auto node = f.engine->get_node("a");
  CONTRACT("failed_batch_rolls_back", !node);
}
NOVA_TEST(corrupt_catalog_rejects_open, "regressions", "") {
  TempDirectory dir;
  {
    Handle db(dir.path());
    auto result = db.query("upsert node Person a");
    CHECK(result.find("\"ok\":true") != std::string::npos);
  }
  {
    std::ofstream out(current_catalog(dir.path()), std::ios::binary | std::ios::trunc);
    out << "broken";
    CHECK(out.good());
  }
  auto handle = graphdb_open_with_config(dir.path().c_str(), 600000, 10);
  bool rejected = handle == nullptr;
  graphdb_close(handle);
  CHECK(rejected);
}
NOVA_TEST(parameter_equals_round_trip, "regressions", "") {
  TempDirectory dir;
  Handle db(dir.path());
  auto result = db.query("upsert node Person a set value=$value", R"({"value":"a=b"})");
  auto stored = db.query("get node a");
  CHECK(result.find("\"ok\":true") != std::string::npos &&
        stored.find("\"value\":\"a=b\"") != std::string::npos);
}
NOVA_TEST(parameter_newline_round_trip, "regressions", "") {
  auto params = parse_property_list(R"({"value":"line1\nline2"})");
  CHECK(params.count("value") == 1);
  CHECK(std::get<std::string>(params.at("value")) == "line1\nline2");
}
NOVA_TEST(double_json_round_trip, "regressions", "") {
  CHECK(property_to_json(Property{1.23456789}) == "1.23456789");
}
NOVA_TEST(negative_query_bounds_rejected, "regressions", "") {
  EngineFixture f;
  auto result = f.engine->execute_dsl("walk from a depth -1 limit -1");
  CONTRACT("negative_query_bounds_rejected", !result.ok);
}
NOVA_TEST(pressure_trim_ignores_ttl, "regressions", "") {
  TempDirectory dir;
  Handle db(dir.path());
  auto added = db.query("upsert node Person a");
  CHECK(added.find("\"ok\":true") != std::string::npos);
  auto result = take(graphdb_trim_memory(db.value));
  CHECK(result.find("\"ok\":true") != std::string::npos);
  CONTRACT("pressure_trim_ignores_ttl", result.find("\"evicted\":1") != std::string::npos);
}
NOVA_TEST(promote_delete_reclaims_payload, "regressions", "") {
  EngineFixture f;
  indexed_cold_node(f);
  auto node = f.engine->get_node("a");
  CHECK(node);
  auto removed = f.engine->delete_node("a");
  CHECK(removed.ok);
  QueryContext context;
  CHECK(f.engine->collect_garbage(100, context));
  CONTRACT("promote_delete_reclaims_payload", std::filesystem::is_empty(f.dir.path() / "cold"));
}
NOVA_TEST(find_propagates_cold_read_failure, "regressions", "") {
  EngineFixture f;
  indexed_cold_node(f);
  auto record = f.hot->get("n/a");
  CHECK(record);
  auto removed = f.disk->remove(record->disk_key);
  CHECK(removed.ok);
  auto found = f.engine->find_nodes("Person", "email", std::string("a@nova.test"), 10);
  CHECK(!found);
}
