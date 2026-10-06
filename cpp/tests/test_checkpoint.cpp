#include "TestSupport.hpp"
#include "graphdb/Recovery.hpp"
using namespace graphdb;
using namespace nova_test;
NOVA_TEST(current_retains_previous_catalog_and_wal_coverage, "checkpoint", "") {
  TempDirectory dir;
  {
    Handle db(dir.path());
    CHECK(db.query("upsert node N a; upsert node N b; upsert edge E a -> b").find("\"ok\":true") !=
          std::string::npos);
    CHECK(take(graphdb_checkpoint(db.value)).find("\"ok\":true") != std::string::npos);
    CHECK(db.query("upsert node N c").find("\"ok\":true") != std::string::npos);
  }
  auto recovered = Recovery::open(dir.path());
  CHECK(recovered);
  CHECK(recovered.value->root().previous.has_value());
  CHECK(!recovered.value->root().sealed.empty());
  CHECK(std::filesystem::exists(dir.path() / "CURRENT"));
  Handle reopened(dir.path());
  CHECK(reopened.query("get node c").find("\"ok\":true") != std::string::npos);
}
#include "ModelFileIO.hpp"
class RootSyncFailure : public ModelFileIO {
  bool root_renamed = false;

public:
  bool armed = false;
  Status rename(const std::filesystem::path &from, const std::filesystem::path &to) override {
    auto result = ModelFileIO::rename(from, to);
    if (armed && result.ok && to.filename() == "CURRENT")
      root_renamed = true;
    return result;
  }
  Status sync_directory(const std::filesystem::path &path) override {
    if (root_renamed) {
      root_renamed = false;
      return Status::Error("injected root directory sync", ErrorCode::ioFailure);
    }
    return ModelFileIO::sync_directory(path);
  }
};
NOVA_TEST(failed_root_publication_fences_and_retains_acknowledged_wal, "checkpoint", "") {
  TempDirectory dir;
  auto io = std::make_shared<RootSyncFailure>();
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
  CHECK(engine.upsert_node("N", "a", {}).ok);
  io->armed = true;
  CHECK(!engine.checkpoint().ok);
  CHECK(!engine.upsert_node("N", "b", {}).ok);
  CHECK(!engine.checkpoint().ok);
  io->power_loss();
  auto restored = Recovery::open(dir.path(), io);
  CHECK(restored);
  auto other = std::make_shared<MemoryHotStore>();
  GraphEngine reopened(other, disk, config, nullptr, restored.value);
  CHECK(reopened.recover().ok);
  CHECK(reopened.get_node("a"));
  CHECK(!reopened.get_node("b"));
}
NOVA_TEST(checkpoint_and_writer_interleaving_retains_every_commit, "checkpoint", "") {
  TempDirectory dir;
  auto root = Recovery::open(dir.path());
  CHECK(root);
  EngineConfig config;
  config.database_dir = dir.path();
  config.checkpoint_on_destroy = false;
  auto hot = std::make_shared<MemoryHotStore>();
  auto disk = std::make_shared<FileDiskStore>(dir.path() / "cold");
  GraphEngine engine(hot, disk, config, nullptr, root.value);
  CHECK(engine.recover().ok);
  std::exception_ptr failure;
  std::thread writer([&] {
    try {
      for (int i = 0; i < 20; ++i)
        CHECK(engine.upsert_node("N", std::to_string(i), {}).ok);
    } catch (...) {
      failure = std::current_exception();
    }
  });
  for (int i = 0; i < 8; ++i)
    CHECK(engine.checkpoint().ok);
  writer.join();
  if (failure)
    std::rethrow_exception(failure);
  auto opened = Recovery::open(dir.path());
  CHECK(opened);
  auto recovered = std::make_shared<MemoryHotStore>();
  GraphEngine after(recovered, disk, config, nullptr, opened.value);
  CHECK(after.recover().ok);
  for (int i = 0; i < 20; ++i)
    CHECK(after.get_node(std::to_string(i)));
}
