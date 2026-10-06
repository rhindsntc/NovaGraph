#include "Campaign.hpp"
#include "graphdb/DatabaseLock.hpp"
#include <fstream>
using namespace graphdb;
using namespace nova_test;
int main(int argc, char **argv) {
  try {
    if (argc < 3)
      throw Failure("usage: worker model count | worker write/verify/trace path [fault]");
    std::string mode = argv[1];
    if (mode == "model-seed") {
      auto seed = std::stoull(argv[2]);
      auto baseline = model_trace(0, 0);
      model_trace(seed, 1 + seed % baseline.events);
      return 0;
    }
    if (mode == "model") {
      auto count = std::stoull(argv[2]);
      auto baseline = model_trace(0, 0);
      size_t crashed = 0;
      for (size_t seed = 0; seed < count; ++seed) {
        auto fault = 1 + seed % baseline.events;
        try {
          auto result = model_trace(seed, fault);
          crashed += result.crashed;
        } catch (const std::exception &error) {
          std::cerr << "FAILING_SEED " << seed << " FAULT " << fault << " " << error.what()
                    << std::endl;
          return 1;
        }
        if ((seed + 1) % 100 == 0)
          std::cerr << "modeled traces " << seed + 1 << "/" << count << std::endl;
      }
      std::cout << "{\"modelTraces\":" << count << ",\"crashes\":" << crashed
                << ",\"boundaries\":" << baseline.events
                << ",\"acknowledgedLosses\":0,\"partialTransactions\":0}" << std::endl;
      return 0;
    }
    auto dir = std::filesystem::path(argv[2]);
    auto lock = DatabaseLock::acquire(dir);
    CHECK(lock);
    auto io = std::make_shared<FaultIO<PosixFileIO>>();
    auto root = Recovery::open(dir, io);
    CHECK(root);
    EngineConfig config;
    config.database_dir = dir;
    config.file_io = io;
    config.checkpoint_on_destroy = false;
    auto hot = std::make_shared<MemoryHotStore>();
    auto disk = std::make_shared<FileDiskStore>(dir / "cold", io);
    GraphEngine engine(hot, disk, config, nullptr, root.value);
    CHECK(engine.recover().ok);
    if (mode == "verify") {
      std::cout << "{\"epoch\":" << inspect(engine) << ",\"indexesEquivalent\":true}" << std::endl;
      return 0;
    }
    io->signal_crash = true;
    io->print_events = mode == "trace";
    io->crash_at = argc > 3 ? std::stoull(argv[3]) : 0;
    io->armed = true;
    for (int epoch = 0; epoch < 8; ++epoch) {
      QueryContext context;
      auto result = engine.commit_batch(epoch_batch(epoch), context);
      CHECK(result);
      std::cout << "ACK " << epoch << std::endl;
      maintain(engine, epoch);
    }
    ::_exit(0); // Deliberately bypass every destructor, including the directory owner.
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
