#include "TestSupport.hpp"
#include <array>
#include <charconv>
#include <cstdlib>
#include <random>
#include <set>
#include <tuple>

using namespace graphdb;
using namespace nova_test;
namespace {
constexpr size_t NodeCount = 16, Steps = 200;
using EdgeKey = std::tuple<std::string, std::string, std::string>;
struct Node { std::string label; PropertyMap properties; };

// Independent oracle: no Nova keys, postings, comparison or traversal helpers.
struct Model {
  std::map<std::string, Node> nodes;
  std::map<EdgeKey, PropertyMap> edges;
  bool apply(const Mutation &mutation) {
    return std::visit([&](const auto &m) -> bool {
      using T = std::decay_t<decltype(m)>;
      if constexpr (std::is_same_v<T, UpsertNode>) {
        nodes[m.id] = {m.label, m.properties};
      } else if constexpr (std::is_same_v<T, UpsertEdge>) {
        if (!nodes.count(m.from) || !nodes.count(m.to)) return false;
        edges[{m.from, m.type, m.to}] = m.properties;
      } else if constexpr (std::is_same_v<T, DeleteNode>) {
        if (!nodes.erase(m.id)) return false;
        std::erase_if(edges, [&](const auto &entry) {
          return std::get<0>(entry.first) == m.id || std::get<2>(entry.first) == m.id;
        });
      } else if constexpr (std::is_same_v<T, DeleteEdge>) {
        if (!edges.erase({m.from, m.type, m.to})) return false;
      } else {
        throw Failure("unsupported reference mutation");
      }
      return true;
    }, mutation);
  }
  bool batch(const std::vector<Mutation> &mutations) {
    auto candidate = *this;
    for (const auto &m : mutations) if (!candidate.apply(m)) return false;
    *this = std::move(candidate);
    return true;
  }
  std::set<std::string> reachable(const std::string &start, const std::string &type,
                                  size_t depth, bool inbound) const {
    std::set<std::string> visited{start}, frontier{start}, result;
    for (size_t d = 0; d < depth && !frontier.empty(); ++d) {
      std::set<std::string> next;
      for (const auto &[edge, props] : edges) {
        const auto &[from, kind, to] = edge;
        if (kind != type || !frontier.count(inbound ? to : from)) continue;
        const auto &target = inbound ? from : to;
        if (visited.insert(target).second) { next.insert(target); result.insert(target); }
      }
      frontier = std::move(next);
    }
    return result;
  }
};
std::string id(size_t n) { return "n" + std::to_string(n); }
void check_node(const GraphObject &actual, const std::string &key, const Node &want) {
  CHECK(actual.kind == ObjectKind::Node && actual.id == key);
  CHECK(actual.label_or_type == want.label && actual.properties == want.properties);
}
std::set<std::string> node_ids(const std::vector<GraphObject> &objects, const Model &model) {
  std::set<std::string> ids;
  for (const auto &object : objects) {
    auto node = model.nodes.find(object.id);
    CHECK(node != model.nodes.end());
    check_node(object, node->first, node->second);
    CHECK(ids.insert(object.id).second); // duplicates cannot disappear in set comparison
  }
  return ids;
}
void verify(GraphEngine &engine, const Model &model) {
  auto sentinel = engine.get_node("absent");
  CHECK(!sentinel && sentinel.status.code == ErrorCode::notFound);
  for (const auto *label : {"A", "B"}) for (int64_t value = 0; value < 3; ++value) {
    std::set<std::string> expected;
    for (const auto &[key, node] : model.nodes) {
      auto v = node.properties.find("v");
      if (node.label == label && v != node.properties.end() && v->second == Property(value))
        expected.insert(key);
    }
    // v is indexed; u is an identical scalar with no property index.
    for (const auto *property : {"v", "u"}) {
      auto found = engine.find_nodes(label, property, value, NodeCount + 1);
      CHECK(found); CHECK(node_ids(found.value, model) == expected);
    }
  }
  for (const auto &[start, node] : model.nodes) for (const auto *type : {"E", "F"}) {
    for (bool inbound : {false, true}) {
      auto found = inbound ? engine.traverse_in(start, type, 3, NodeCount + 1)
                           : engine.traverse_out(start, type, 3, NodeCount + 1);
      CHECK(found); CHECK(node_ids(found.value.nodes, model) == model.reachable(start, type, 3, inbound));
    }
  }
  for (size_t i = 0; i < NodeCount; ++i) {
    auto key = id(i); auto node = engine.get_node(key); auto want = model.nodes.find(key);
    if (want == model.nodes.end()) CHECK(!node && node.status.code == ErrorCode::notFound);
    else { CHECK(node); check_node(node.value, key, want->second); }
    for (const auto *type : {"E", "F"}) {
      auto rejected_edge = engine.get_edge(key, type, "absent");
      CHECK(!rejected_edge && rejected_edge.status.code == ErrorCode::notFound);
    }
    for (size_t j = 0; j < NodeCount; ++j) for (const auto *type : {"E", "F"}) {
      auto to = id(j); auto edge = engine.get_edge(key, type, to);
      auto expected = model.edges.find({key, type, to});
      if (expected == model.edges.end()) CHECK(!edge && edge.status.code == ErrorCode::notFound);
      else {
        CHECK(edge); CHECK(edge.value.kind == ObjectKind::Edge);
        CHECK(edge.value.from == key && edge.value.to == to && edge.value.label_or_type == type);
        CHECK(edge.value.properties == expected->second);
      }
    }
  }
}
PropertyMap properties(uint32_t n) {
  if (n % 5 == 0) return {{"text", std::string("replacement")}};
  return {{"v", int64_t(n % 3)}, {"u", int64_t(n % 3)}, {"text", std::to_string(n)}};
}
void run_seed(uint32_t seed) {
  struct Fixture {
    TempDirectory dir;
    EngineConfig config;
    std::shared_ptr<MemoryHotStore> hot;
    std::shared_ptr<FileDiskStore> disk;
    std::unique_ptr<GraphEngine> engine;
  } f;
  // Bootstrap CURRENT before creating cold/: a nonempty unknown directory must be rejected.
  f.config.database_dir = f.dir.path();
  f.config.checkpoint_on_destroy = false;
  Model model; std::mt19937 random(seed);
  size_t step = 0;
  auto commit = [&](std::vector<Mutation> mutations) {
    bool expected = model.batch(mutations);
    QueryContext context;
    auto result = f.engine->commit_batch({new_transaction_id(), std::move(mutations)}, context);
    CHECK(static_cast<bool>(result) == expected);
    if (!expected) CHECK(result.status.code == ErrorCode::notFound);
  };
  auto reopen = [&] {
    f.engine.reset(); f.hot = std::make_shared<MemoryHotStore>();
    auto recovered = Recovery::open(f.dir.path(), f.config.file_io);
    if (!recovered) throw Failure(recovered.status.message);
    f.disk = std::make_shared<FileDiskStore>(f.dir.path() / "cold");
    f.engine = std::make_unique<GraphEngine>(f.hot, f.disk, f.config, nullptr, recovered.value);
    CHECK(f.engine->recover().ok);
  };
  try {
    reopen();
    CHECK(f.engine->create_node_property_index("A", "v").ok);
    CHECK(f.engine->create_node_property_index("B", "v").ok);
    // Guarantee cycles, self-loops, typed edges and staged endpoint creation.
    commit({UpsertNode{"A", id(0), properties(1)}, UpsertNode{"B", id(1), properties(2)},
            UpsertEdge{"E", id(0), id(1), {}}, UpsertEdge{"E", id(1), id(0), {}},
            UpsertEdge{"F", id(0), id(0), properties(3)}});
    verify(*f.engine, model);
    for (step = 0; step < Steps; ++step) {
      // Modulo a specified mt19937 output is portable across standard libraries.
      auto a = id(random() % NodeCount), b = id(random() % NodeCount);
      auto type = random() % 2 ? "E" : "F";
      auto label = random() % 2 ? "A" : "B";
      auto props = properties(random()); auto op = random() % 10;
      std::cout << "MODEL seed=" << seed << " step=" << step << " op=" << op
                << " a=" << a << " b=" << b << " type=" << type << " label=" << label
                << " props=" << properties_to_json(props) << '\n';
      switch (op) {
      case 0: case 1: case 2: commit({UpsertNode{label, a, props}}); break;
      case 3: commit({UpsertEdge{type, a, b, props}}); break;
      case 4: commit({DeleteNode{a}}); break;
      case 5: commit({DeleteEdge{a, type, b}}); break;
      case 6: // A new/replaced node must also roll back when a later statement fails.
        commit({UpsertNode{label, a, props}, UpsertEdge{type, a, "absent", {}}}); break;
      case 7:
        commit({UpsertNode{label, a, props}, UpsertNode{"B", b, props},
                UpsertEdge{type, a, b, props}}); break;
      case 8: {
        QueryContext context; auto trimmed = f.engine->trim_memory(0, context);
        CHECK(trimmed); CHECK(f.hot->memory_usage().hot_payload_bytes == 0);
        // Rebuild while cold; verify() then checks and promotes actual disk payloads.
        CHECK(f.engine->rebuild_indexes().ok); break;
      }
      case 9:
        // Alternate checkpoint recovery and recovery with committed WAL suffix.
        if (step % 2 == 0) CHECK(f.engine->checkpoint().ok);
        reopen();
        CHECK(f.engine->has_node_property_index("A", "v"));
        CHECK(f.engine->has_node_property_index("B", "v")); break;
      }
      verify(*f.engine, model);
    }
    QueryContext context; CHECK(f.engine->trim_memory(0, context));
    CHECK(f.engine->checkpoint().ok); reopen(); verify(*f.engine, model);
  } catch (const std::exception &error) {
    throw Failure("MODEL failing seed=" + std::to_string(seed) + " step=" +
                  std::to_string(step) + ": " + error.what());
  }
}
} // namespace

// Catches incorrect replacement/cascade/rollback/index/traversal or recovered payloads.
NOVA_TEST(randomized_graph_matches_independent_model, "model", "") {
  if (const char *value = std::getenv("NOVA_MODEL_SEED")) {
    uint32_t seed; std::string_view text(value);
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), seed);
    CHECK(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size());
    run_seed(seed);
  } else {
    for (uint32_t seed : {1u, 42u, 0x4e6f7661u, 0xdeadbeefu}) run_seed(seed);
  }
}

// Verify the oracle observes real engine divergence rather than comparing itself.
NOVA_TEST(graph_model_detects_unexpected_mutation, "model", "") {
  EngineFixture f; Model model;
  CHECK(f.engine->upsert_node("A", "n0", properties(1)).ok);
  CHECK(model.apply(UpsertNode{"A", "n0", properties(1)}));
  verify(*f.engine, model);
  CHECK(f.engine->upsert_node("A", "n0", properties(2)).ok);
  bool detected = false;
  try { verify(*f.engine, model); } catch (const Failure &) { detected = true; }
  CHECK(detected);
}

// A failed batch must not leave its invalid edge outside the normal ID universe.
NOVA_TEST(graph_model_detects_dangling_edge_leak, "model", "") {
  EngineFixture f; Model model;
  auto config = f.config; config.legacy_permissive_endpoints = true;
  GraphEngine permissive(f.hot, f.disk, config);
  CHECK(permissive.upsert_node("A", "n0", properties(1)).ok);
  CHECK(model.apply(UpsertNode{"A", "n0", properties(1)}));
  verify(permissive, model);
  CHECK(permissive.upsert_edge("E", "n0", "absent", {}).ok);
  bool detected = false;
  try { verify(permissive, model); } catch (const Failure &) { detected = true; }
  CHECK(detected);
}
