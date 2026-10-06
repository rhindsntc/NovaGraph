#pragma once
#include "../TestSupport.hpp"
#include "FaultIO.hpp"
#include <iomanip>
namespace nova_test {
inline graphdb::TransactionBatch epoch_batch(int epoch, size_t seed = 0) {
  using namespace graphdb;
  TransactionBatch batch{new_transaction_id(), {}};
  if (epoch == 0)
    batch.mutations.emplace_back(CreateIndex{"N", "v"});
  if (epoch % 3 == 1)
    batch.mutations.emplace_back(DeleteNode{"right"});
  batch.mutations.emplace_back(UpsertNode{
      "N", "left", {{"v", int64_t(epoch)}, {"payload", std::string(60 + epoch % 7, 'x')}}});
  batch.mutations.emplace_back(UpsertNode{"N", "right", {{"v", int64_t(epoch)}}});
  batch.mutations.emplace_back(UpsertEdge{"E", "left", "right", {{"v", int64_t(epoch)}}});
  for (auto &mutation : batch.mutations)
    if (auto node = std::get_if<UpsertNode>(&mutation)) {
      node->properties["seed"] = int64_t(seed);
      node->properties["largeInt"] = INT64_MAX - int64_t(seed);
      node->properties["flag"] = bool(seed % 2);
      node->properties["real"] = double(seed) / 8;
    }
  if (seed % 2) {
    auto end = batch.mutations.size();
    std::swap(batch.mutations[end - 2], batch.mutations[end - 3]);
  }
  return batch;
}
inline void maintain(graphdb::GraphEngine &engine, int epoch) {
  graphdb::QueryContext context;
  if (epoch % 3 == 1) {
    CHECK(engine.get_node("left"));
    CHECK(engine.rebuild_indexes().ok);
  } else {
    CHECK(engine.trim_memory(0, context));
    CHECK(engine.checkpoint().ok);
    if (epoch % 3 == 2)
      CHECK(engine.collect_garbage(100, context));
  }
}
// Independent invariants: pair+edge presence/epoch, indexed versus scanned result,
// both adjacency directions, and equality to rebuilt authoritative index state.
inline int inspect(graphdb::GraphEngine &engine, bool rebuild = true) {
  using namespace graphdb;
  auto left = engine.get_node("left"), right = engine.get_node("right"),
       edge = engine.get_edge("left", "E", "right");
  if (!left) {
    CHECK(left.status.code == ErrorCode::notFound);
    CHECK(!right && right.status.code == ErrorCode::notFound);
    CHECK(!edge && edge.status.code == ErrorCode::notFound);
    return -1;
  }
  CHECK(right && edge);
  auto epoch = std::get<int64_t>(left.value.properties.at("v"));
  CHECK(std::get<int64_t>(right.value.properties.at("v")) == epoch);
  CHECK(std::get<int64_t>(edge.value.properties.at("v")) == epoch);
  auto indexed = engine.find_nodes("N", "v", epoch, 10),
       scanned = engine.find_nodes("N", "v", epoch, 10, ">=");
  CHECK(indexed && scanned);
  CHECK(indexed.value.size() == 2 && scanned.value.size() == 2);
  auto out = engine.walk_out("left", "E", 1, 10), in = engine.walk_in("right", "E", 1, 10);
  CHECK(out && in);
  CHECK(out.value.size() == 1 && in.value.size() == 1);
  if (rebuild) {
    auto indexes = engine.hot_store()->dump_indexes();
    CHECK(engine.rebuild_indexes().ok);
    CHECK(engine.hot_store()->dump_indexes() == indexes);
  }
  return int(epoch);
}
struct CampaignResult {
  size_t events;
  bool crashed;
  std::string point;
};
inline CampaignResult model_trace(size_t seed, size_t crash_at) {
  using namespace graphdb;
  TempDirectory dir;
  auto io = std::make_shared<FaultIO<ModelFileIO>>();
  auto root = Recovery::open(dir.path(), io);
  CHECK(root);
  EngineConfig config;
  config.database_dir = dir.path();
  config.file_io = io;
  config.checkpoint_on_destroy = false;
  auto disk = std::make_shared<FileDiskStore>(dir.path() / "cold", io);
  int acknowledged = -1, attempting = -1;
  bool crashed = false;
  io->armed = true;
  io->crash_at = crash_at;
  try {
    auto hot = std::make_shared<MemoryHotStore>();
    GraphEngine engine(hot, disk, config, nullptr, root.value);
    CHECK(engine.recover().ok);
    for (int i = 0; i < 8; ++i) {
      attempting = i;
      QueryContext context;
      auto batch = epoch_batch(i, seed);
      auto result = engine.commit_batch(batch, context);
      CHECK(result);
      acknowledged = i;
      maintain(engine, i);
    }
  } catch (const PowerLoss &) {
    crashed = true;
  }
  io->armed = false;
  auto count = io->counter;
  auto point = io->last_event;
  root.value.reset();
  io->power_loss();
  auto opened = Recovery::open(dir.path(), io);
  if (!opened)
    throw Failure("seed=" + std::to_string(seed) + " point=" + point +
                  " open=" + opened.status.message);
  auto hot = std::make_shared<MemoryHotStore>();
  GraphEngine recovered(hot, disk, config, nullptr, opened.value);
  CHECK(recovered.recover().ok);
  int actual = inspect(recovered);
  CHECK(actual >= acknowledged);
  CHECK(actual == acknowledged || actual == attempting);
  if (actual >= 0) {
    auto left = recovered.get_node("left");
    CHECK(std::get<int64_t>(left.value.properties.at("seed")) == int64_t(seed));
    CHECK(std::get<int64_t>(left.value.properties.at("largeInt")) == INT64_MAX - int64_t(seed));
    CHECK(std::get<double>(left.value.properties.at("real")) == double(seed) / 8);
    CHECK(std::get<bool>(left.value.properties.at("flag")) == bool(seed % 2));
  }
  // The recovered append point must remain usable after any allowed tail repair.
  QueryContext context;
  CHECK(recovered.commit_batch(
      {new_transaction_id(), {UpsertNode{"Probe", "after", {{"seed", int64_t(seed)}}}}}, context));
  io->power_loss();
  auto again = Recovery::open(dir.path(), io);
  CHECK(again);
  auto final_hot = std::make_shared<MemoryHotStore>();
  GraphEngine final(final_hot, disk, config, nullptr, again.value);
  CHECK(final.recover().ok);
  CHECK(final.get_node("after"));
  CHECK(inspect(final) == actual);
  return {count, crashed, point};
}
} // namespace nova_test
