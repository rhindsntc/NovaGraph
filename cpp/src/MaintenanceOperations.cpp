#include "graphdb/MaintenanceOperations.hpp"
#include "graphdb/DatabaseLock.hpp"
#include "graphdb/GraphEngine.hpp"
#include <cerrno>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#else
#include <stdio.h>
#endif
namespace graphdb {
namespace {
constexpr size_t max_snapshot = 64 * 1024 * 1024;
Status bad(std::string s) { return Status::Error(std::move(s), ErrorCode::corruptData); }
class ReadDisk : public DiskStore {
  std::filesystem::path root_;
  std::shared_ptr<FileIO> io_;

public:
  ReadDisk(std::filesystem::path root, std::shared_ptr<FileIO> io)
      : root_(std::move(root)), io_(std::move(io)) {}
  Result<GraphObject> read(const std::string &name) override {
    if (std::filesystem::path(name).filename() != name || name == "." || name == ".." ||
        name.empty())
      return {bad("invalid payload name"), {}};
    auto b = io_->read(root_ / name, kMaxObjectBytes);
    if (!b)
      return {b.status, {}};
    return deserialize_object_binary(b.value.data(), b.value.size());
  }
  Result<std::string> write(const std::string &, const GraphObject &) override {
    return {bad("read-only maintenance source"), {}};
  }
  Status remove(const std::string &) override { return bad("read-only maintenance source"); }
};
Status budget(const CatalogData &data) {
  size_t bytes = 0;
  std::set<std::string> nodes, keys;
  for (const auto &r : data.records) {
    auto encoded = serialize_object_binary(r.object);
    if (encoded.size() > max_snapshot - bytes)
      return Status::Error("maintenance snapshot exceeds 64 MiB", ErrorCode::limitExceeded);
    bytes += encoded.size();
    if (!keys.insert(object_key(r.object.kind, r.object.id)).second)
      return bad("duplicate record identity");
    if (r.object.kind == ObjectKind::Node)
      nodes.insert(r.object.id);
  }
  for (const auto &r : data.records)
    if (r.object.kind == ObjectKind::Edge &&
        (!nodes.count(r.object.from) || !nodes.count(r.object.to)))
      return bad("edge has missing endpoint; explicit application repair required");
  return Status::OK();
}
Status hydrate(CatalogData &data, DiskStore &disk, bool verify_indexes) {
  size_t bytes = 0;
  for (auto &r : data.records) {
    if (r.tier == StorageTier::Cold) {
      if (r.payload_bytes > max_snapshot - bytes)
        return Status::Error("maintenance snapshot exceeds 64 MiB", ErrorCode::limitExceeded);
      auto o = read_manifest(disk, r);
      if (!o)
        return o.status;
      r.object = std::move(o.value);
      r.tier = StorageTier::Hot;
      r.disk_key.clear();
    }
    auto n = serialize_object_binary(r.object).size();
    if (n > max_snapshot - bytes)
      return Status::Error("maintenance snapshot exceeds 64 MiB", ErrorCode::limitExceeded);
    bytes += n;
  }
  auto valid = budget(data);
  if (!valid.ok)
    return valid;
  MemoryHotStore rebuilt;
  rebuilt.load_records(data.records, data.declared_indexes);
  auto indexes = rebuilt.dump_indexes();
  if (verify_indexes && indexes != data.indexes)
    return bad("persisted indexes differ from authoritative payloads; run rebuild-indexes");
  data.indexes = std::move(indexes);
  return Status::OK();
}
MaintenanceReport report(const CatalogData &d, bool tail = false) {
  MaintenanceReport out;
  out.format = "CURRENT3/catalog3/record3/WAL4";
  out.generation = d.generation;
  out.committed_lsn = d.last_lsn;
  out.indexes = d.declared_indexes.size();
  out.tail_repair_needed = tail;
  for (const auto &r : d.records)
    if (r.object.kind == ObjectKind::Node)
      ++out.nodes;
    else
      ++out.edges;
  return out;
}
Result<CatalogData> read_current(const std::filesystem::path &dir, std::shared_ptr<FileIO> io,
                                 bool repair, bool *tail = nullptr) {
  auto mode = repair ? CatalogReadMode::rebuild_derived : CatalogReadMode::strict;
  auto root = Recovery::open(dir, io, false, mode);
  if (!root)
    return {root.status, {}};
  ReadDisk disk(dir / "cold", io);
  if (root.value->root().previous) {
    auto prior = Catalog::load(dir / root.value->root().previous->filename, io, mode);
    if (!prior)
      return prior;
    auto checked = hydrate(prior.value, disk, !repair);
    if (!checked.ok)
      return {checked, {}};
  }
  auto data = root.value->catalog();
  auto checked = hydrate(data, disk, !repair);
  if (!checked.ok)
    return {checked, {}};
  if (tail)
    *tail = root.value->wal()->needs_repair();
  auto replayed = MaintenanceAccess::replay(std::move(data), root.value->transactions());
  if (!replayed)
    return replayed;
  replayed.value.last_lsn = root.value->wal()->committed_lsn();
  return replayed;
}
Result<std::filesystem::path> destination_path(const std::filesystem::path &source,
                                               const std::filesystem::path &destination) {
  if (destination.empty() || destination.native().find('\0') != std::string::npos)
    return {Status::Error("destination must be an absent directory", ErrorCode::invalidArgument),
            {}};
  auto dest = std::filesystem::weakly_canonical(std::filesystem::absolute(destination));
  auto src = std::filesystem::canonical(source);
  auto relative = dest.lexically_relative(src);
  if (dest == src || (!relative.empty() && *relative.begin() != ".."))
    return {Status::Error("destination aliases or is inside source", ErrorCode::invalidArgument),
            {}};
  if (std::filesystem::symlink_status(destination).type() !=
          std::filesystem::file_type::not_found ||
      std::filesystem::exists(dest))
    return {Status::Error("destination already exists", ErrorCode::conflict), {}};
  if (!std::filesystem::is_directory(dest.parent_path()))
    return {Status::Error("destination parent must exist", ErrorCode::notFound), {}};
  return {Status::OK(), dest};
}
Status persist(std::filesystem::path dir, CatalogData data, std::shared_ptr<FileIO> io) {
  FileDiskStore disk(dir / "cold", io);
  for (auto &r : data.records) {
    auto file = disk.write(object_key(r.object.kind, r.object.id), r.object);
    if (!file)
      return file.status;
    auto descriptor = describe_payload(disk, file.value);
    if (!descriptor)
      return descriptor.status;
    r.tier = StorageTier::Cold;
    r.disk_key = file.value;
    r.payload_bytes = descriptor.value.bytes;
    r.payload_crc = descriptor.value.crc;
    r.object.properties.clear();
    io->fault_point("maintenance.after_copy");
  }
  auto synced = io->sync_directory(dir / "cold");
  if (!synced.ok)
    return synced;
  return Recovery::install_snapshot(dir, std::move(data), io);
}
Status exclusive_publish(const std::filesystem::path &from, const std::filesystem::path &to) {
  int rc;
#ifdef __APPLE__
  rc = renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#elif defined(__linux__)
  rc = syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE);
#else
  return Status::Error("exclusive directory publication unavailable on this platform",
                       ErrorCode::unsupportedVersion);
#endif
  if (rc)
    return Status::Error("exclusive destination publication failed",
                         errno == EEXIST ? ErrorCode::conflict : ErrorCode::ioFailure,
                         {to.string(), errno});
  return Status::OK();
}
} // namespace
Result<CatalogData> MaintenanceAccess::replay(CatalogData data,
                                              const std::vector<RecoveredTransaction> &groups,
                                              bool legacy) {
  auto hot = std::make_shared<MemoryHotStore>();
  hot->load_records(data.records, data.declared_indexes);
  hot->load_indexes(data.indexes);
  EngineConfig cfg;
  cfg.enable_wal = false;
  cfg.legacy_permissive_endpoints = legacy;
  cfg.checkpoint_on_destroy = false;
  GraphEngine view(hot, std::make_shared<ReadDisk>("", default_file_io()), cfg);
  for (const auto &group : groups)
    for (const auto &mutation : group.batch.mutations) {
      auto s = view.apply_mutation(mutation);
      const bool missing_delete = legacy && s.code == ErrorCode::notFound &&
                                  (std::holds_alternative<DeleteNode>(mutation) ||
                                   std::holds_alternative<DeleteEdge>(mutation));
      if (!s.ok && !missing_delete)
        return {s, {}};
    }
  data.records = hot->dump_records();
  data.declared_indexes = hot->dump_declared_indexes();
  data.indexes = hot->dump_indexes();
  auto valid = budget(data);
  if (!valid.ok)
    return {valid, {}};
  return {Status::OK(), std::move(data)};
}
Result<CatalogData> MaintenanceAccess::snapshot(GraphEngine &engine) {
  CatalogData data;
  data.records = engine.hot_->dump_records();
  data.declared_indexes = engine.hot_->dump_declared_indexes();
  data.indexes = engine.hot_->dump_indexes();
  data.last_lsn = engine.committed_lsn_;
  if (!engine.recovery_ || engine.write_fenced_ || engine.recovery_->fenced())
    return {Status::Error("backup requires healthy managed database", ErrorCode::closed), {}};
  data.database_id = engine.recovery_->root().database_id;
  auto valid = hydrate(data, *engine.disk_, true);
  if (!valid.ok)
    return {valid, {}};
  return {Status::OK(), std::move(data)};
}
Status GraphEngine::backup(const std::filesystem::path &destination) {
  if (reentrant())
    return Status::Error("backup cannot run inside snapshot", ErrorCode::conflict);
  auto gate = acquire();
  auto data = MaintenanceAccess::snapshot(*this);
  if (!data)
    return data.status;
  return publish_backup(std::move(data.value), config_.database_dir, destination, config_.file_io)
      .status;
}
Result<MaintenanceReport> publish_backup(CatalogData data, const std::filesystem::path &source,
                                         const std::filesystem::path &destination,
                                         std::shared_ptr<FileIO> io) {
  auto dest = destination_path(source, destination);
  if (!dest)
    return {dest.status, {}};
  auto stage = dest.value.parent_path() / (".nova-stage-" + new_transaction_id());
  if (!std::filesystem::create_directory(stage))
    return {Status::Error("cannot create unique staging directory", ErrorCode::conflict), {}};
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove_all(path, e);
    }
  } cleanup{stage};
  auto ownership = DatabaseLock::acquire(stage);
  if (!ownership)
    return {ownership.status, {}};
  auto synced = io->sync_directory(stage.parent_path());
  if (!synced.ok)
    return {synced, {}};
  data.generation = 1;
  auto written = persist(stage, data, io);
  if (!written.ok)
    return {written, {}};
  auto verify = read_current(stage, io, false);
  if (!verify)
    return {verify.status, {}};
  io->fault_point("maintenance.after_verify");
  auto out = report(verify.value);
  synced = io->sync_file(stage / ".nova.lock");
  if (!synced.ok)
    return {synced, {}};
  synced = io->sync_directory(stage);
  if (!synced.ok)
    return {synced, {}};
  io->fault_point("maintenance.before_publish");
  auto published = exclusive_publish(stage, dest.value);
  if (!published.ok)
    return {published, {}};
  io->fault_point("maintenance.after_publish");
  synced = io->sync_directory(dest.value.parent_path());
  if (!synced.ok)
    return {synced, {}};
  io->fault_point("maintenance.after_parent_sync");
  return {Status::OK(), std::move(out)};
}
Result<MaintenanceReport> maintain(const std::string &command, const std::filesystem::path &source,
                                   const std::filesystem::path &destination,
                                   std::shared_ptr<FileIO> io) {
  try {
    if (command != "inspect" && command != "verify" && command != "backup" &&
        command != "restore" && command != "rebuild-indexes" && command != "migrate-v1")
      return {Status::Error("unknown maintenance command", ErrorCode::invalidArgument), {}};
    if (source.empty() || source.native().find('\0') != std::string::npos ||
        !std::filesystem::is_directory(source))
      return {Status::Error("source directory does not exist", ErrorCode::notFound), {}};
    auto ownership = DatabaseLock::acquire(source, false);
    if (!ownership)
      return {ownership.status, {}};
    auto dir = ownership.value->directory();
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(dir / "cold")))
      return {bad("cold directory must not be a symbolic link"), {}};
    auto current = io->read(dir / "CURRENT", 1024 * 1024);
    if (!current && current.status.code != ErrorCode::notFound)
      return {current.status, {}};
    bool legacy = !current;
    if (legacy) {
      if (command != "migrate-v1" && command != "inspect")
        return {Status::Error("legacy source requires explicit migrate-v1 into a new directory",
                              ErrorCode::unsupportedVersion),
                {}};
      auto data = read_legacy_v1(dir, io);
      if (!data)
        return {data.status, {}};
      if (command == "migrate-v1") {
        auto result = publish_backup(std::move(data.value), dir, destination, io);
        if (result)
          result.value.warnings.push_back(
              "Legacy timestamps retain their original clock basis; WAL-only upserts have unknown "
              "modification time encoded as zero. Lost v1 data and ambiguous delimiter collisions "
              "cannot be reconstructed.");
        return result;
      }
      auto out = report(data.value);
      out.format = "legacy-v1";
      out.warnings.push_back("Stop all legacy writers before migration; v1 has no transaction "
                             "durability or reliable writer lock.");
      return {Status::OK(), std::move(out)};
    }
    if (command == "migrate-v1")
      return {Status::Error("source is not legacy v1", ErrorCode::unsupportedVersion), {}};
    bool tail = false;
    auto data = read_current(dir, io, command == "rebuild-indexes", &tail);
    if (!data)
      return {data.status, {}};
    if (command == "backup" || command == "restore")
      return publish_backup(std::move(data.value), dir, destination, io);
    if (command == "rebuild-indexes") {
      if (data.value.generation == UINT64_MAX)
        return {Status::Error("generation exhausted", ErrorCode::limitExceeded), {}};
      ++data.value.generation;
      io->fault_point("maintenance.before_repair");
      auto written = persist(dir, data.value, io);
      if (!written.ok)
        return {written, {}};
      io->fault_point("maintenance.after_repair");
      auto verified = read_current(dir, io, false);
      if (!verified)
        return {verified.status, {}};
      return {Status::OK(), report(verified.value)};
    }
    return {Status::OK(), report(data.value, tail)};
  } catch (const FaultInterruption &) {
    throw;
  } catch (const std::filesystem::filesystem_error &e) {
    return {Status::Error(e.what(), ErrorCode::ioFailure, {e.path1().string(), e.code().value()}),
            {}};
  } catch (const std::exception &e) {
    return {Status::Error(e.what(), ErrorCode::invalidArgument), {}};
  }
}
std::string maintenance_json(const MaintenanceReport &r) {
  std::string out =
      "{\"ok\":true,\"format\":\"" + json_escape(r.format) +
      "\",\"generation\":" + std::to_string(r.generation) +
      ",\"committedLSN\":" + std::to_string(r.committed_lsn) +
      ",\"nodes\":" + std::to_string(r.nodes) + ",\"edges\":" + std::to_string(r.edges) +
      ",\"indexes\":" + std::to_string(r.indexes) +
      ",\"tailRepairNeeded\":" + (r.tail_repair_needed ? "true" : "false") + ",\"warnings\":[";
  for (size_t i = 0; i < r.warnings.size(); ++i) {
    if (i)
      out += ",";
    out += "\"" + json_escape(r.warnings[i]) + "\"";
  }
  return out + "]}";
}
} // namespace graphdb
