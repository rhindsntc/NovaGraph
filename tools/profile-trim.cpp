// Diagnostic only: see docs-web/content/benchmarks-data.md for build instructions and scope.
#include "graphdb/GraphEngine.hpp"
#include <chrono>
#include <iostream>

using namespace graphdb;
using Clock = std::chrono::steady_clock;
struct Metric {
  size_t calls{0};
  double seconds{0};
};
class TimedIO : public PosixFileIO {
public:
  Metric writes, reads, files, directories;
  template <class F> auto timed(Metric &metric, F operation) {
    auto start = Clock::now();
    auto result = operation();
    ++metric.calls;
    metric.seconds += std::chrono::duration<double>(Clock::now() - start).count();
    return result;
  }
  Status write_all(const std::filesystem::path &path, const std::vector<uint8_t> &bytes,
                   WriteMode mode) override {
    return timed(writes, [&] { return PosixFileIO::write_all(path, bytes, mode); });
  }
  Result<std::vector<uint8_t>> read(const std::filesystem::path &path, size_t maximum) override {
    return timed(reads, [&] { return PosixFileIO::read(path, maximum); });
  }
  Status sync_file(const std::filesystem::path &path) override {
    return timed(files, [&] { return PosixFileIO::sync_file(path); });
  }
  Status sync_files(const std::vector<std::filesystem::path> &paths) override {
    return timed(files, [&] { return PosixFileIO::sync_files(paths); });
  }
  Status sync_directory(const std::filesystem::path &path) override {
    return timed(directories, [&] { return PosixFileIO::sync_directory(path); });
  }
};
int main(int argc, char **argv) {
  auto root = std::filesystem::temp_directory_path() / ("nova-trim-profile-" + new_transaction_id());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  } cleanup{root};
  try {
    size_t consumed = 0;
    auto nodes = argc == 1 ? 2000 : std::stoul(argv[1], &consumed);
    if (argc > 2 || nodes == 0 || nodes > 2048 ||
        (argc == 2 && consumed != std::string(argv[1]).size()))
      throw std::runtime_error("usage: profile-trim [node-count: 1..2048]");
    auto io = std::make_shared<TimedIO>();
    auto hot = std::make_shared<MemoryHotStore>();
    auto disk = std::make_shared<FileDiskStore>(root / "cold", io);
    auto require = [](Status status) {
      if (!status.ok)
        throw std::runtime_error(status.message);
    };
    require(hot->create_node_property_index("User", "email"));
    for (size_t i = 0; i < nodes; ++i) {
      GraphObject object;
      object.id = "u:" + std::to_string(i);
      object.label_or_type = "User";
      object.properties = {{"name", "User " + std::to_string(i)},
                           {"email", "user" + std::to_string(i) + "@nova.bench"},
                           {"score", double(i * 10)}};
      require(hot->put_hot(object_key(object.kind, object.id), object));
    }
    for (size_t i = 0; i + 1 < nodes; ++i) {
      GraphObject object;
      object.kind = ObjectKind::Edge;
      object.from = "u:" + std::to_string(i);
      object.to = "u:" + std::to_string(i + 1);
      object.id = edge_id(object.from, "FOLLOWS", object.to);
      object.label_or_type = "FOLLOWS";
      object.properties = {{"weight", 1.0}};
      require(hot->put_hot(object_key(object.kind, object.id), object));
    }
    EngineConfig config;
    config.database_dir = root;
    config.file_io = io;
    config.checkpoint_on_destroy = false;
    GraphEngine engine(hot, disk, config);
    QueryContext context;
    auto start = Clock::now();
    auto result = engine.trim_memory(0, context);
    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    require(result.status);
    if (result.value.evicted_count != nodes * 2 - 1 || result.value.unmet_bytes != 0)
      throw std::runtime_error("trim did not evict the complete workload");
    std::cout << "{\"nodes\":" << nodes << ",\"records\":" << result.value.evicted_count
              << ",\"seconds\":" << elapsed << ",\"io\":{";
    bool first = true;
    for (auto [name, metric] : std::initializer_list<std::pair<const char *, Metric>>{
             {"writes", io->writes}, {"reads", io->reads}, {"fileSync", io->files},
             {"directorySync", io->directories}}) {
      if (!first)
        std::cout << ',';
      first = false;
      std::cout << '"' << name << "\":{\"calls\":" << metric.calls
                << ",\"seconds\":" << metric.seconds << '}';
    }
    std::cout << "},\"evictedBytes\":" << result.value.evicted_bytes
              << ",\"unmetBytes\":" << result.value.unmet_bytes << "}\n";
  } catch (const std::exception &error) {
    std::cerr << "Trim profile failed: " << error.what() << '\n';
    return 1;
  }
}
