#pragma once

#include "graphdb/Catalog.hpp"
#include "graphdb/QueryContext.hpp"
#include "graphdb/Recovery.hpp"
#include "graphdb/Storage.hpp"
#include "graphdb/Wal.hpp"
#include <functional>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace graphdb {

struct EngineConfig {
  std::filesystem::path database_dir{"/tmp/nova-graph"};
  int64_t hot_ttl_ms{600000};
  int64_t sweep_interval_ms{30000};
  size_t sweep_batch_size{4096};
  bool strict_activity_on_read{true};
  bool enable_wal{true};
  bool legacy_permissive_endpoints{false};
  MemoryLimits memory;
  QueryOptions query_options;
  std::function<int64_t()> wall_clock = wall_millis;
  std::function<int64_t()> monotonic_clock = now_millis;
  bool checkpoint_on_destroy{true};
  std::shared_ptr<FileIO> file_io = default_file_io();
};

struct QueryResult {
  bool ok{true};
  std::string json;
  std::string error;
  Status status;
  std::optional<CommitReceipt> receipt;
  QueryResult(bool success, std::string data, std::string message, Status failure = Status::OK())
      : ok(success), json(std::move(data)), error(std::move(message)), status(std::move(failure)) {
    if (!ok) {
      if (status.ok)
        status = Status::Error(error, ErrorCode::parseError);
      else
        status.message = error;
    }
  }
};

class GraphEngine {
public:
  GraphEngine(std::shared_ptr<HotStore> hot_store, std::shared_ptr<DiskStore> disk_store,
              EngineConfig config = {}, std::shared_ptr<WalManager> wal = nullptr,
              std::shared_ptr<Recovery> recovery = nullptr);
  ~GraphEngine();

  GraphEngine(const GraphEngine &) = delete;
  GraphEngine &operator=(const GraphEngine &) = delete;

  Status recover();
  Status checkpoint();
  // Request limits govern admission only; maintenance does not use query workspace.
  Status checkpoint(QueryContext &context);
  Status backup(const std::filesystem::path &destination);
  static bool snapshot_owned_by_current_thread(const GraphEngine *engine) {
    return std::find(snapshot_owners_.begin(),snapshot_owners_.end(),engine)!=snapshot_owners_.end();
  }
  bool snapshot_on_current_thread() const { return reentrant(); }
  void disable_close_checkpoint() { checkpoint_on_destroy_ = false; }
  void enable_close_checkpoint() { checkpoint_on_destroy_ = true; }

  Status start_tiering_worker();
  void stop_tiering_worker();
  size_t sweep_once();

  Status upsert_node(std::string label, std::string id, PropertyMap properties);
  Status upsert_edge(std::string type, std::string from, std::string to, PropertyMap properties);
  Status delete_node(std::string id);
  Status delete_edge(std::string from, std::string type, std::string to);

  Result<GraphObject> get_node(const std::string &id);
  Result<GraphObject> get_edge(const std::string &from, const std::string &type,
                               const std::string &to);
  Result<std::vector<GraphObject>> find_nodes(std::string label, std::string where_key,
                                              Property where_value, size_t limit,
                                              std::string where_op = "=");
  Result<std::vector<GraphObject>> walk_out(std::string from, std::string edge_type, size_t depth,
                                            size_t limit);
  Result<std::vector<GraphObject>> walk_in(std::string to, std::string edge_type, size_t depth,
                                           size_t limit);
  Result<TraversalResult> traverse_out(std::string from, std::string edge_type, size_t depth,
                                        size_t limit, bool include_paths = false);
  Result<TraversalResult> traverse_in(std::string to, std::string edge_type, size_t depth,
                                       size_t limit, bool include_paths = false);
  Status create_node_property_index(std::string label, std::string property_name);
  bool has_node_property_index(const std::string &label, const std::string &property_name) const;
  Status rebuild_indexes();

  QueryResult execute_dsl(std::string query, const PropertyMap &parameters = {});
  QueryResult execute_dsl(std::string query, const PropertyMap &parameters, QueryContext &context);

  QueryResult inspect(unsigned kind, const std::string &cursor, size_t limit, QueryContext &);

  QueryOptions configured_query_options() const { return query_options(); }
  std::shared_ptr<HotStore> hot_store() const { return hot_; }
  std::shared_ptr<DiskStore> disk_store() const { return disk_; }

public:
  Result<CommitReceipt> commit_batch(const TransactionBatch &, QueryContext &);
  class ReadSnapshot {
    GraphEngine &engine_;
    QueryContext owned_context_;
    QueryContext &context_;
    std::unique_lock<std::timed_mutex> lock_;
    QueryContext *previous_;
    bool previous_read_only_;

    friend class GraphEngine;
    explicit ReadSnapshot(GraphEngine &, QueryContext * = nullptr);

  public:
    ~ReadSnapshot();
    QueryResult execute_dsl(std::string query, const PropertyMap &parameters = {});
    ReadSnapshot(const ReadSnapshot &) = delete;
    ReadSnapshot &operator=(const ReadSnapshot &) = delete;
    Result<GraphObject> get_node(const std::string &id) { return engine_.get_node_unlocked(id); }
    Result<GraphObject> get_edge(const std::string &from, const std::string &type,
                                 const std::string &to) {
      return engine_.get_edge_unlocked(from, type, to);
    }
    Result<std::vector<GraphObject>> find_nodes(std::string label, std::string property,
                                                Property value, size_t limit) {
      return engine_.find_nodes_unlocked(std::move(label), std::move(property), std::move(value),
                                         limit);
    }
  };
  Result<TrimResult> trim_memory(size_t target_bytes, QueryContext &);
  Result<GarbageResult> collect_garbage(size_t max_files, QueryContext &);
  Result<std::shared_ptr<void>> pin_node(const std::string &);
  MemoryUsage memory_usage() const {
    auto lock = acquire();
    return hot_->memory_usage();
  }
  ReadSnapshot read_snapshot() { return ReadSnapshot(*this); }
  ReadSnapshot read_snapshot(QueryContext &context) { return ReadSnapshot(*this,&context); }

private:
  friend class EngineAccess;
  QueryContext *active_query_{nullptr};
  QueryOptions query_options() const;
  struct QueryScope {
    GraphEngine &engine; QueryContext *previous;
    QueryScope(GraphEngine &e, QueryContext &c) : engine(e), previous(e.active_query_) { e.active_query_=&c; }
    ~QueryScope() { engine.active_query_=previous; }
  };
  friend class MaintenanceAccess;
  mutable std::timed_mutex operation_mu_;
  static thread_local std::vector<const GraphEngine *> snapshot_owners_;
  std::unique_lock<std::timed_mutex> acquire(QueryContext *context = nullptr) const;
  bool reentrant() const {
    return std::find(snapshot_owners_.begin(), snapshot_owners_.end(), this) !=
           snapshot_owners_.end();
  }
  Result<CommitReceipt> publish(const TransactionBatch &, HotStore &, QueryContext &);
  QueryResult run_program(const TransactionBatch &,
                          const std::function<QueryResult(GraphEngine &)> &);
  Status apply_mutation(const Mutation &);
  Status check_transaction_workspace(const TransactionBatch &) const;
  Status check_state_budget(const HotStore &) const;
  Result<TrimResult> trim_unlocked(size_t, QueryContext &, bool ttl);
  std::array<size_t, 2> gc_offsets_{};
  size_t gc_area_{0};
  uint64_t committed_lsn_{0};
  const std::string inspection_instance_{new_transaction_id()};
  Status recover_unlocked();
  Status checkpoint_unlocked();
  size_t sweep_once_unlocked();
  Status upsert_node_unlocked(std::string label, std::string id, PropertyMap properties,
                              std::optional<int64_t> modified = {});
  Status upsert_edge_unlocked(std::string type, std::string from, std::string to,
                              PropertyMap properties, std::optional<int64_t> modified = {});
  Status delete_node_unlocked(std::string id);
  Status delete_edge_unlocked(std::string from, std::string type, std::string to);
  Result<GraphObject> get_node_unlocked(const std::string &id);
  Result<GraphObject> get_edge_unlocked(const std::string &from, const std::string &type,
                                        const std::string &to);
  Result<std::vector<GraphObject>> find_nodes_unlocked(std::string label, std::string where_key,
                                                       Property where_value, size_t limit,
                                                       std::string where_op = "=");
  Result<TraversalResult> traverse_unlocked(const std::string &, const std::string &, size_t,
                                            size_t, bool inbound, bool include_paths,
                                            bool measure_output = false);
  Result<std::vector<GraphObject>> walk_out_unlocked(std::string from, std::string edge_type,
                                                     size_t depth, size_t limit);
  Result<std::vector<GraphObject>> walk_in_unlocked(std::string to, std::string edge_type,
                                                    size_t depth, size_t limit);
  Status create_node_property_index_unlocked(std::string label, std::string property_name);
  bool has_node_property_index_unlocked(const std::string &label,
                                        const std::string &property_name) const;
  Status rebuild_indexes_unlocked();
  Result<GraphObject> get_by_key(const std::string &key);
  Status put_object(GraphObject object, std::optional<int64_t> modified = {});
  void tiering_loop();

  std::shared_ptr<HotStore> hot_;
  std::shared_ptr<DiskStore> disk_;
  EngineConfig config_;
  std::shared_ptr<WalManager> wal_;
  std::shared_ptr<Recovery> recovery_;
  std::filesystem::path catalog_path_;
  uint64_t last_checkpoint_lsn_{0};
  std::atomic<bool> checkpoint_on_destroy_{true};
  std::atomic<bool> write_fenced_{false};
  std::atomic<bool> running_{false};
  std::condition_variable cv_;
  std::mutex cv_mu_;
  std::thread worker_;
};

} // namespace graphdb
