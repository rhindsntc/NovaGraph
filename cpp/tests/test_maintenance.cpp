#include "TestSupport.hpp"
#include "graphdb/MaintenanceOperations.hpp"
#include <fstream>
using namespace graphdb;
using namespace nova_test;
namespace {
std::map<std::string, std::vector<uint8_t>> bytes_at(const std::filesystem::path &p) {
  std::map<std::string, std::vector<uint8_t>> out;
  for (const auto &e : std::filesystem::recursive_directory_iterator(p))
    if (e.is_regular_file()) {
      auto r = default_file_io()->read(e.path(), 64 * 1024 * 1024);
      CHECK(r);
      out[e.path().lexically_relative(p).string()] = r.value;
    }
  return out;
}
void seed(const std::filesystem::path &p) {
  Handle db(p);
  CHECK(db.query("upsert node N a set v=7; upsert node N b set v=8; upsert edge E a -> b set v=9")
            .find("\"ok\":true") != std::string::npos);
  CHECK(take(graphdb_rebuild_indexes(db.value)).find("true") != std::string::npos);
  CHECK(db.query("create index on N(v)").find("\"ok\":true") != std::string::npos);
  CHECK(take(graphdb_trim_memory(db.value)).find("true") != std::string::npos);
}
} // namespace
NOVA_TEST(backup_restore_preserves_cold_graph_and_source_bytes, "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "source", backup = d.path() / "backup", restored = d.path() / "restored";
  seed(source);
  auto before = bytes_at(source);
  auto v = maintain("verify", source);
  CHECK(v);
  CHECK(v.value.nodes == 2);
  CHECK(v.value.edges == 1);
  CHECK(maintain("backup", source, backup));
  CHECK(bytes_at(source) == before);
  CHECK(maintain("restore", backup, restored));
  CHECK(maintain("verify", restored));
  Handle db(restored);
  CHECK(db.query("find nodes N where v = 7").find("\"id\":\"a\"") != std::string::npos);
  CHECK(db.query("walk from a over E depth 1").find("\"id\":\"b\"") != std::string::npos);
}
NOVA_TEST(maintenance_rejects_alias_existing_destination_busy_and_missing_source, "maintenance",
          "") {
  TempDirectory d;
  auto source = d.path() / "source";
  seed(source);
  auto before = bytes_at(source);
  CHECK(!maintain("backup", source, source));
  CHECK(!maintain("backup", source, source / "child"));
  std::filesystem::create_directory_symlink(source, d.path() / "alias");
  CHECK(!maintain("restore", source, d.path() / "alias"));
  CHECK(!maintain("verify", d.path() / "missing"));
  CHECK(!std::filesystem::exists(d.path() / "missing"));
  {
    Handle db(source);
    CHECK(maintain("verify", source).status.code == ErrorCode::busy);
  }
  CHECK(!maintain("restore", source, d.path()));
}
NOVA_TEST(offline_verify_does_not_repair_tail_and_rejects_missing_payload, "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "source";
  seed(source);
  auto wal = active_wal(source);
  CHECK(default_file_io()->write_all(wal, {4, 0}, WriteMode::append).ok);
  auto before = bytes_at(source);
  auto verified = maintain("verify", source);
  CHECK(verified);
  CHECK(verified.value.tail_repair_needed);
  CHECK(bytes_at(source) == before);
  auto cat = Catalog::load(current_catalog(source));
  CHECK(cat);
  CHECK(!cat.value.records.empty());
  auto cold = cat.value.records.front().disk_key;
  CHECK(!cold.empty());
  std::filesystem::remove(source / "cold" / cold);
  auto broken = bytes_at(source);
  CHECK(!maintain("verify", source));
  CHECK(!maintain("backup", source, d.path() / "bad"));
  CHECK(bytes_at(source) == broken);
  CHECK(!std::filesystem::exists(d.path() / "bad"));
}
#include "LegacyFixture.hpp"
NOVA_TEST(legacy_import_preserves_scalars_cold_indexes_edges_and_source, "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "v1", dest = d.path() / "new";
  legacy_fixture(source);
  auto before = bytes_at(source);
  auto imported = maintain("migrate-v1", source, dest);
  CHECK(imported);
  CHECK(imported.value.nodes == 3);
  CHECK(imported.value.edges == 1);
  CHECK(bytes_at(source) == before);
  auto cat = Recovery::open(dest);
  CHECK(cat);
  CHECK(cat.value->catalog().declared_indexes.size() == 1);
  Handle db(dest);
  auto result = db.query("find nodes N where v = 7");
  CHECK(result.find("9223372036854775807") != std::string::npos);
  CHECK(result.find("\\u0000") != std::string::npos);
  CHECK(result.find("\"zero\":0") != std::string::npos);
  CHECK(db.query("walk from a over E depth 1").find("\"id\":\"b\"") != std::string::npos);
}
NOVA_TEST(legacy_import_rejects_damaged_or_missing_data_without_destination, "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "v1";
  legacy_fixture(source);
  std::filesystem::remove(source / "cold" / "a.rec");
  auto before = bytes_at(source);
  CHECK(!maintain("migrate-v1", source, d.path() / "bad"));
  CHECK(!std::filesystem::exists(d.path() / "bad"));
  CHECK(bytes_at(source) == before);
}
NOVA_TEST(online_backup_captures_committed_state_while_handle_is_open, "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "source", dest = d.path() / "backup";
  Handle db(source);
  CHECK(db.query("upsert node N a set v=7").find("true") != std::string::npos);
  auto result = take(graphdb_backup(db.value, dest.c_str()));
  CHECK(result.find("\"ok\":true") != std::string::npos);
  CHECK(db.query("upsert node N b set v=8").find("true") != std::string::npos);
  auto verified = maintain("verify", dest);
  CHECK(verified);
  CHECK(verified.value.nodes == 1);
  CHECK(take(graphdb_maintenance("verify", source.c_str(), nullptr)).find("\"code\":\"busy\"") !=
        std::string::npos);
}
NOVA_TEST(explicit_rebuild_repairs_cold_index_mismatch_without_losing_records, "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "source";
  seed(source);
  auto root = Recovery::open(source);
  CHECK(root);
  auto data = root.value->catalog();
  data.indexes.sections[1].clear();
  CHECK(Recovery::install_snapshot(source, data, default_file_io()).ok);
  CHECK(!maintain("verify", source));
  CHECK(maintain("rebuild-indexes", source));
  CHECK(maintain("verify", source));
  Handle db(source);
  CHECK(db.query("find nodes N where v = 7").find("\"id\":\"a\"") != std::string::npos);
}
NOVA_TEST(ordinary_open_rejects_legacy_without_adding_files, "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "v1";
  legacy_fixture(source);
  auto before = bytes_at(source);
  auto opened = graphdb_open_result(source.c_str(), 1000, 10);
  CHECK(!opened.handle);
  CHECK(take(opened.error).find("unsupportedVersion") != std::string::npos);
  CHECK(bytes_at(source) == before);
}
#include "faults/FaultIO.hpp"
#include <sys/wait.h>
NOVA_TEST(maintenance_sigkill_preserves_source_and_publishes_only_verified_destinations,
          "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "source", v1 = d.path() / "v1";
  seed(source);
  legacy_fixture(v1);
  auto last_backup = d.path() / "last-backup";
  CHECK(maintain("backup", source, last_backup));
  auto backup_bytes = bytes_at(last_backup);
  for (const std::string command : {"backup", "restore", "migrate-v1", "rebuild-indexes"}) {
    auto src = command == "migrate-v1" ? v1 : command == "restore" ? last_backup : source;
    auto trace = std::make_shared<FaultIO<PosixFileIO>>();
    trace->armed = true;
    auto trace_source = src;
    if (command == "rebuild-indexes") {
      trace_source = d.path() / "repair-trace";
      std::filesystem::copy(src, trace_source, std::filesystem::copy_options::recursive);
    }
    CHECK(maintain(command, trace_source, d.path() / ("baseline-" + command), trace));
    auto before = bytes_at(src);
    size_t count = trace->counter;
    CHECK(count > 10);
    for (size_t point = 1; point <= count; ++point) {
      auto dest = d.path() / (command + "-" + std::to_string(point));
      auto active_source = src;
      if (command == "rebuild-indexes") {
        active_source = d.path() / ("repair-" + std::to_string(point));
        std::filesystem::copy(src, active_source, std::filesystem::copy_options::recursive);
      }
      pid_t child = fork();
      CHECK(child >= 0);
      if (child == 0) {
        auto io = std::make_shared<FaultIO<PosixFileIO>>();
        io->armed = true;
        io->signal_crash = true;
        io->crash_at = point;
        auto result = maintain(command, active_source, dest, io);
        _exit(result ? 0 : 2);
      }
      int status;
      CHECK(waitpid(child, &status, 0) == child);
      CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
      if (command == "rebuild-indexes") {
        CHECK(maintain("verify", active_source));
      } else {
        CHECK(bytes_at(src) == before);
        if (std::filesystem::exists(dest)) {
          auto check = maintain("verify", dest);
          CHECK(check);
          CHECK(check.value.nodes == (command == "migrate-v1" ? 3 : 2));
          CHECK(check.value.edges == 1);
        }
      }
      CHECK(bytes_at(last_backup) == backup_bytes);
    }
    std::cout << "maintenance " << command << " SIGKILL boundaries=" << count << '\n';
  }
}
NOVA_TEST(maintenance_rejects_corrupt_catalog_unknown_version_and_legacy_tail, "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "source";
  seed(source);
  auto catalog = current_catalog(source);
  auto io = default_file_io();
  auto b = io->read(catalog, 64 * 1024 * 1024).value;
  b.back() ^= 1;
  CHECK(io->write_all(catalog, b, WriteMode::replace).ok);
  auto before = bytes_at(source);
  CHECK(!maintain("rebuild-indexes", source));
  CHECK(!maintain("restore", source, d.path() / "bad"));
  CHECK(bytes_at(source) == before);
  auto v1 = d.path() / "v1";
  legacy_fixture(v1);
  CHECK(io->write_all(v1 / "wal.log", {4}, WriteMode::append).ok);
  before = bytes_at(v1);
  CHECK(!maintain("migrate-v1", v1, d.path() / "new"));
  CHECK(bytes_at(v1) == before);
  CHECK(!std::filesystem::exists(d.path() / "new"));
  auto future = d.path() / "future";
  seed(future);
  auto root = io->read(future / "CURRENT", 10000).value;
  root[4] = 99;
  CHECK(io->write_all(future / "CURRENT", root, WriteMode::replace).ok);
  CHECK(maintain("restore", future, d.path() / "new").status.code == ErrorCode::unsupportedVersion);
}
NOVA_TEST(maintenance_reports_unknown_legacy_timestamps_and_rejects_external_payload_directory,
          "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "v1";
  legacy_fixture(source);
  auto migrated = maintain("migrate-v1", source, d.path() / "new");
  CHECK(migrated);
  CHECK(!migrated.value.warnings.empty());
  std::filesystem::rename(source / "cold", d.path() / "outside");
  std::filesystem::create_directory_symlink(d.path() / "outside", source / "cold");
  CHECK(!maintain("migrate-v1", source, d.path() / "bad"));
  CHECK(!std::filesystem::exists(d.path() / "bad"));
}
NOVA_TEST(backup_never_replaces_destination_created_during_copy, "maintenance", "") {
  struct RaceIO : PosixFileIO {
    std::filesystem::path target;
    bool fired = false;
    void fault_point(std::string_view name) override {
      if (!fired && name == "maintenance.before_publish") {
        fired = true;
        std::filesystem::create_directory(target);
        std::ofstream(target / "keep") << "untouched";
      }
    }
  };
  TempDirectory d;
  auto source = d.path() / "source", dest = d.path() / "dest";
  seed(source);
  auto io = std::make_shared<RaceIO>();
  io->target = dest;
  auto result = maintain("backup", source, dest, io);
  CHECK(!result);
  CHECK(result.status.code == ErrorCode::conflict);
  CHECK(std::filesystem::exists(dest / "keep"));
  CHECK(!std::filesystem::exists(dest / "CURRENT"));
}
NOVA_TEST(legacy_replay_allows_recorded_missing_deletes_and_forward_edges, "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "v1";
  legacy_fixture(source);
  auto io = default_file_io();
  auto append = [&](uint64_t lsn, uint8_t op, std::vector<uint8_t> payload) {
    std::vector<uint8_t> f;
    encode_u64(f, lsn);
    encode_u8(f, op);
    f.insert(f.end(), payload.begin(), payload.end());
    std::vector<uint8_t> wire;
    encode_u32(wire, f.size());
    wire.insert(wire.end(), f.begin(), f.end());
    encode_u32(wire, crc32_compute(f.data(), f.size()));
    CHECK(io->write_all(source / "wal.log", wire, WriteMode::append).ok);
  };
  std::vector<uint8_t> p;
  encode_string(p, "absent");
  append(2, 3, p);
  p.clear();
  encode_string(p, "unknown");
  encode_string(p, "E");
  encode_string(p, "absent");
  append(3, 4, p);
  p.clear();
  encode_string(p, "F");
  encode_string(p, "d");
  encode_string(p, "e");
  encode_properties(p, {});
  append(4, 2, p);
  uint64_t lsn = 5;
  for (auto id : {"d", "e"}) {
    p.clear();
    encode_string(p, "N");
    encode_string(p, id);
    encode_properties(p, {});
    append(lsn++, 1, p);
  }
  auto imported = maintain("migrate-v1", source, d.path() / "new");
  CHECK(imported);
  CHECK(imported.value.nodes == 5);
  CHECK(imported.value.edges == 2);
  CHECK(maintain("verify", d.path() / "new"));
}
#include "graphdb/DatabaseLock.hpp"
NOVA_TEST(offline_current_maintenance_rejects_missing_ownership_file, "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "source";
  seed(source);
  std::filesystem::remove(source / ".nova.lock");
  auto before = bytes_at(source);
  auto lock = DatabaseLock::acquire(source, false);
  CHECK(!lock);
  CHECK(!maintain("rebuild-indexes", source));
  CHECK(bytes_at(source) == before);
  Handle owner(source);
  CHECK(maintain("verify", source).status.code == ErrorCode::busy);
}
NOVA_TEST(legacy_import_rejects_ambiguous_delimiter_edge_history, "maintenance", "") {
  TempDirectory d;
  auto source = d.path() / "v1";
  legacy_fixture(source);
  auto io = default_file_io();
  auto append = [&](uint64_t lsn, uint8_t op, std::vector<uint8_t> payload) {
    std::vector<uint8_t> f;
    encode_u64(f, lsn);
    encode_u8(f, op);
    f.insert(f.end(), payload.begin(), payload.end());
    std::vector<uint8_t> wire;
    encode_u32(wire, f.size());
    wire.insert(wire.end(), f.begin(), f.end());
    encode_u32(wire, crc32_compute(f.data(), f.size()));
    CHECK(io->write_all(source / "wal.log", wire, WriteMode::append).ok);
  };
  std::vector<uint8_t> p;
  encode_string(p, "N");
  encode_string(p, "a->b");
  encode_properties(p, {});
  append(2, 1, p);
  p.clear();
  encode_string(p, "b->c");
  encode_string(p, "a");
  encode_string(p, "b");
  encode_properties(p, {});
  append(3, 2, p);
  p.clear();
  encode_string(p, "c");
  encode_string(p, "a->b");
  encode_string(p, "b");
  encode_properties(p, {});
  append(4, 2, p);
  auto before = bytes_at(source);
  CHECK(!maintain("migrate-v1", source, d.path() / "new"));
  CHECK(!std::filesystem::exists(d.path() / "new"));
  CHECK(bytes_at(source) == before);
}
