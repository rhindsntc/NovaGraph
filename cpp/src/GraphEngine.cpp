#include "graphdb/GraphEngine.hpp"
#include "graphdb/DslParser.hpp"
#include "graphdb/ValueCodec.hpp"
#include "graphdb/QueryJson.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <thread>
#include <unordered_set>

namespace graphdb {

GraphEngine::GraphEngine(std::shared_ptr<HotStore> hot_store, std::shared_ptr<DiskStore> disk_store,
                         EngineConfig config, std::shared_ptr<WalManager> wal,
                         std::shared_ptr<Recovery> recovery)
    : hot_(std::move(hot_store)), disk_(std::move(disk_store)), config_(std::move(config)),
      wal_(recovery ? recovery->wal() : std::move(wal)), recovery_(std::move(recovery)),
      catalog_path_(config_.database_dir / "catalog.db"),
      checkpoint_on_destroy_(config_.checkpoint_on_destroy) {
  auto valid=config_.query_options.validate(); if(!valid.ok) throw std::invalid_argument(valid.message);
}

GraphEngine::~GraphEngine() {
  stop_tiering_worker();
  if (checkpoint_on_destroy_) {
    try {
      checkpoint();
    } catch (...) {
    }
  }
}

Status GraphEngine::recover_unlocked() {
  checkpoint_on_destroy_ = false;
  if (recovery_) {
    const auto &catalog = recovery_->catalog();
    auto staged = std::make_shared<MemoryHotStore>();
    staged->load_records(catalog.records, catalog.declared_indexes);
    staged->load_indexes(catalog.indexes);
    auto config = config_;
    config.checkpoint_on_destroy = false;
    GraphEngine view(staged, disk_, config);
    for (const auto &group : recovery_->transactions())
      for (const auto &mutation : group.batch.mutations) {
        auto applied = view.apply_mutation(mutation);
        if (!applied.ok)
          return applied;
      }
    staged->reset_activity(config_.monotonic_clock());
    if (staged->memory_usage().metadata_bytes > config_.memory.metadata_bytes)
      return Status::Error("recovered metadata exceeds budget", ErrorCode::limitExceeded);
    // Recovery residency may differ from precrash residency. Spill only after
    // complete logical validation, leaving all existing recovery bytes intact.
    while (staged->memory_usage().hot_payload_bytes > config_.memory.hot_payload_bytes) {
      auto candidates = staged->cold_candidates(INT64_MAX, 1);
      if (candidates.empty())
        return Status::Error("recovery cannot release hot payload", ErrorCode::limitExceeded);
      const auto &record = candidates.front();
      auto key = object_key(record.object.kind, record.object.id);
      auto written = disk_->write(key, record.object);
      if (!written)
        return written.status;
      auto descriptor = describe_payload(*disk_, written.value);
      if (!descriptor)
        return descriptor.status;
      if (!staged->move_to_cold_if_version(key, record.object.version, written.value,
                                           descriptor.value.bytes, descriptor.value.crc,
                                           record.activity_version))
        return Status::Error("recovery residency conflict", ErrorCode::conflict);
    }
    auto budget = check_state_budget(*staged);
    if (!budget.ok)
      return budget;
    auto repaired = recovery_->repair_tail();
    if (!repaired.ok)
      return repaired;
    hot_->swap_state(*staged);
    committed_lsn_ = wal_->committed_lsn();
    last_checkpoint_lsn_ = catalog.last_lsn;
    checkpoint_on_destroy_ = config_.checkpoint_on_destroy;
    return Status::OK();
  }
  auto catalog = Catalog::load(catalog_path_, config_.file_io);
  if (!catalog && catalog.status.code != ErrorCode::notFound)
    return catalog.status;
  if (catalog) {
    hot_->load_records(std::move(catalog.value.records), std::move(catalog.value.declared_indexes));
    hot_->load_indexes(catalog.value.indexes);
    last_checkpoint_lsn_ = catalog.value.last_lsn;
  }

  if (wal_) {
    auto wal_res = wal_->recover_entries();
    if (!wal_res)
      return wal_res.status;
    if (wal_res) {
      // A rotated WAL is only meaningful together with its checkpoint catalog.
      // A database that never checkpointed can still replay complete history.
      if (!catalog &&
          ((wal_->first_lsn() != 1) ||
           std::any_of(wal_res.value.begin(), wal_res.value.end(),
                       [](const auto &entry) { return entry.op == WalOpType::Checkpoint; })))
        return Status::Error("WAL history requires a missing catalog", ErrorCode::corruptData,
                             {catalog_path_.string()});
      for (auto &entry : wal_res.value) {
        if (entry.lsn <= last_checkpoint_lsn_)
          continue;

        switch (entry.op) {
        case WalOpType::UpsertNode: {
          GraphObject obj;
          obj.kind = ObjectKind::Node;
          obj.id = std::move(entry.id);
          obj.label_or_type = std::move(entry.label_or_type);
          obj.properties = std::move(entry.properties);
          auto applied = put_object(std::move(obj), entry.modified_ms);
          if (!applied.ok)
            return applied;
          break;
        }
        case WalOpType::UpsertEdge: {
          GraphObject obj;
          obj.kind = ObjectKind::Edge;
          obj.id = edge_id(entry.from, entry.label_or_type, entry.to);
          obj.label_or_type = std::move(entry.label_or_type);
          obj.from = std::move(entry.from);
          obj.to = std::move(entry.to);
          obj.properties = std::move(entry.properties);
          auto applied = put_object(std::move(obj), entry.modified_ms);
          if (!applied.ok)
            return applied;
          break;
        }
        case WalOpType::DeleteNode: {
          std::vector<std::string> dummy;
          auto applied = hot_->delete_node(entry.id, dummy);
          if (!applied.ok && applied.code != ErrorCode::notFound)
            return applied;
          break;
        }
        case WalOpType::DeleteEdge: {
          std::string dummy;
          auto applied = hot_->delete_edge(entry.from, entry.label_or_type, entry.to, dummy);
          if (!applied.ok && applied.code != ErrorCode::notFound)
            return applied;
          break;
        }
        case WalOpType::CreateIndex: {
          auto applied =
              hot_->create_node_property_index(entry.label_or_type, entry.index_property);
          if (!applied.ok)
            return applied;
          break;
        }
        case WalOpType::Checkpoint:
          last_checkpoint_lsn_ = entry.lsn;
          break;
        }
      }
    }
  }
  if (wal_) {
    auto repaired = wal_->repair_tail();
    if (!repaired.ok)
      return repaired;
  }
  hot_->reset_activity(config_.monotonic_clock());
  checkpoint_on_destroy_ = config_.checkpoint_on_destroy;
  return Status::OK();
}

Status GraphEngine::checkpoint_unlocked() {
  if (write_fenced_ || (wal_ && !wal_->writable()))
    return Status::Error("database writes are fenced; reopen required", ErrorCode::closed);
  if(active_query_) {
    auto usage=hot_->memory_usage();
    active_query_->reserve_work(usage.metadata_bytes*4);
    for(const auto &rec:hot_->dump_records(false)) active_query_->reserve_work(rec.payload_bytes*24);
  }
  auto records = hot_->dump_records();
  auto declared_indexes = hot_->dump_declared_indexes();
  uint64_t current_lsn = wal_ ? wal_->next_lsn() - 1 : 0;

  CatalogData data;
  data.last_lsn = current_lsn;
  data.records = std::move(records);
  data.declared_indexes = std::move(declared_indexes);
  data.indexes = hot_->dump_indexes();
  if (wal_)
    data.database_id = wal_->database_id();
  if (recovery_) {
    auto saved = recovery_->checkpoint(std::move(data));
    if (!saved.ok) {
      if (recovery_->fenced()) {
        write_fenced_ = true;
        checkpoint_on_destroy_ = false;
      }
      return saved;
    }
    wal_ = recovery_->wal();
    last_checkpoint_lsn_ = current_lsn;
    return Status::OK();
  }
  auto st = Catalog::save(catalog_path_, data, config_.file_io);
  if (!st.ok)
    return st;

  last_checkpoint_lsn_ = current_lsn;
  if (wal_) {
    auto truncated = wal_->truncate_after_checkpoint(current_lsn);
    if (!truncated.ok)
      return truncated;
  }
  return Status::OK();
}

Status GraphEngine::start_tiering_worker() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true))
    return Status::OK();
  worker_ = std::thread([this] { tiering_loop(); });
  return Status::OK();
}

void GraphEngine::stop_tiering_worker() {
  {
    std::lock_guard lock(cv_mu_);
    running_.store(false);
  }
  cv_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

void GraphEngine::tiering_loop() {
  while (running_.load()) {
    try {
      sweep_once();
    } catch (...) {
      running_ = false;
      return;
    }
    std::unique_lock<std::mutex> lk(cv_mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(config_.sweep_interval_ms),
                 [this] { return !running_.load(); });
  }
}

Status GraphEngine::put_object(GraphObject object, std::optional<int64_t> modified) {
  if (object.id.empty())
    return Status::Error("object id must not be empty");
  auto key = object_key(object.kind, object.id);
  object.last_modified_ms = modified.value_or(config_.wall_clock());
  object.last_read_ms = object.last_modified_ms;
  auto saved = hot_->put_hot(key, std::move(object));
  if (saved.ok)
    hot_->touch_read(key, config_.wall_clock(), config_.monotonic_clock());
  return saved;
}

Status GraphEngine::upsert_node_unlocked(std::string label, std::string id, PropertyMap properties,
                                         std::optional<int64_t> modified) {
  try {
    validate_identity(label);
    validate_identity(id);
    validate_properties(properties);
  } catch (const std::exception &e) {
    return Status::Error(e.what(), ErrorCode::invalidArgument);
  }
  if (label.empty())
    return Status::Error("node label must not be empty");
  if (id.empty())
    return Status::Error("node id must not be empty");

  GraphObject obj;
  obj.kind = ObjectKind::Node;
  obj.id = std::move(id);
  obj.label_or_type = std::move(label);
  obj.properties = std::move(properties);
  return put_object(std::move(obj), modified);
}

Status GraphEngine::upsert_edge_unlocked(std::string type, std::string from, std::string to,
                                         PropertyMap properties, std::optional<int64_t> modified) {
  try {
    validate_identity(type);
    validate_identity(from);
    validate_identity(to);
    validate_properties(properties);
  } catch (const std::exception &e) {
    return Status::Error(e.what(), ErrorCode::invalidArgument);
  }
  if (type.empty())
    return Status::Error("edge type must not be empty");
  if (from.empty() || to.empty())
    return Status::Error("edge endpoints must not be empty");

  if (!config_.legacy_permissive_endpoints && (!hot_->get(object_key(ObjectKind::Node, from), false) ||
                                               !hot_->get(object_key(ObjectKind::Node, to), false)))
    return Status::Error("edge endpoints must exist", ErrorCode::notFound);
  GraphObject obj;
  obj.kind = ObjectKind::Edge;
  obj.id = edge_id(from, type, to);
  obj.label_or_type = std::move(type);
  obj.from = std::move(from);
  obj.to = std::move(to);
  obj.properties = std::move(properties);
  return put_object(std::move(obj), modified);
}

Status GraphEngine::delete_node_unlocked(std::string id) {
  try {
    validate_identity(id);
  } catch (const std::exception &e) {
    return Status::Error(e.what(), ErrorCode::invalidArgument);
  }
  if (id.empty())
    return Status::Error("node id must not be empty");

  std::vector<std::string> removed_disk_keys;
  auto st = hot_->delete_node(id, removed_disk_keys);
  if (!st.ok)
    return st;

  return Status::OK();
}

Status GraphEngine::delete_edge_unlocked(std::string from, std::string type, std::string to) {
  try {
    validate_identity(from);
    validate_identity(type);
    validate_identity(to);
  } catch (const std::exception &e) {
    return Status::Error(e.what(), ErrorCode::invalidArgument);
  }
  if (from.empty() || type.empty() || to.empty()) {
    return Status::Error("edge endpoints and type must not be empty");
  }

  std::string removed_disk_key;
  auto st = hot_->delete_edge(from, type, to, removed_disk_key);
  if (!st.ok)
    return st;

  return Status::OK();
}

Result<GraphObject> GraphEngine::get_by_key(const std::string &key) {
  QueryContext fallback(query_options());
  QueryScope scope(*this,active_query_ ? *active_query_ : fallback);
  try {
  if(active_query_) active_query_->reserve_work(hot_->query_record_charge(key));
  auto rec = hot_->get(key, false);
  if (!rec)
    return {Status::Error("not found", ErrorCode::notFound), {}};
  auto now = config_.wall_clock();
  if (rec->payload_bytes > config_.memory.query_bytes / 2 ||
      rec->payload_bytes > config_.memory.result_bytes / 6)
    return {Status::Error("record exceeds query/result budget", ErrorCode::limitExceeded), {}};
  if (rec->tier == StorageTier::Cold) {
    auto disk_res = read_manifest(*disk_, *rec);
    active_query_->enforce();
    if (!disk_res)
      return disk_res;
    auto usage = hot_->memory_usage();
    auto promote = Status::OK();
    if (rec->payload_bytes <= config_.memory.hot_payload_bytes &&
        usage.hot_payload_bytes <= config_.memory.hot_payload_bytes - rec->payload_bytes)
      promote = hot_->promote_if_cold(key, disk_res.value, now, config_.monotonic_clock());
    if (!promote.ok)
      return {promote, {}};
    disk_res.value.last_read_ms = now;
    return disk_res;
  }
  if (config_.strict_activity_on_read)
    hot_->touch_read(key, now, config_.monotonic_clock());
  rec = hot_->get(key);
  rec->object.last_read_ms = now;
  return {Status::OK(), rec->object};
  } catch(const QueryFailure &e) {return {e.status,{}};}
}

Result<GraphObject> GraphEngine::get_node_unlocked(const std::string &id) {
  return get_by_key(object_key(ObjectKind::Node, id));
}

Result<GraphObject> GraphEngine::get_edge_unlocked(const std::string &from, const std::string &type,
                                                   const std::string &to) {
  return get_by_key(object_key(ObjectKind::Edge, edge_id(from, type, to)));
}

Result<std::vector<GraphObject>>
GraphEngine::find_nodes_unlocked(std::string label, std::string where_key, Property where_value,
                                 size_t limit, std::string where_op) {
  QueryContext fallback(query_options());
  QueryScope scope(*this,active_query_ ? *active_query_ : fallback);
  try {
    auto &context=*active_query_; context.bounds(0,limit);
    if(limit==0)return {Status::OK(),{}};
    bool indexed=!where_key.empty() && (where_op=="=" || where_op=="==") && hot_->has_node_property_index(label,where_key);
    std::vector<GraphObject> out;
    std::string after;
    while(out.size()<limit) {
      auto key=hot_->next_query_key(indexed ? HotStore::QueryIndex::Property : HotStore::QueryIndex::Label,
                                   label,where_key,where_value,after,context);
      if(!key)break;
      after=*key;
      auto node=get_by_key(*key); if(!node)return {node.status,{}};
      if(!where_key.empty()) {
        auto it=node.value.properties.find(where_key);
        if(it==node.value.properties.end() ? where_op!="!=" : !compare_properties(it->second,where_op,where_value))continue;
      }
      context.reserve_work(sizeof(GraphObject)*2);
      out.push_back(std::move(node.value));
    }
    return {Status::OK(),std::move(out)};
  } catch(const QueryFailure &e) { return {e.status,{}}; }
}

Result<std::vector<GraphObject>> GraphEngine::walk_out_unlocked(std::string from,
    std::string type, size_t depth, size_t limit) {
  auto result = traverse_unlocked(from, type, depth, limit, false, false);
  return {result.status, std::move(result.value.nodes)};
}
Result<std::vector<GraphObject>> GraphEngine::walk_in_unlocked(std::string from,
    std::string type, size_t depth, size_t limit) {
  auto result = traverse_unlocked(from, type, depth, limit, true, false);
  return {result.status, std::move(result.value.nodes)};
}
Result<TraversalResult> GraphEngine::traverse_unlocked(const std::string &from,
    const std::string &type, size_t depth, size_t limit, bool inbound, bool include_paths,
    bool measure_output) {
  QueryContext fallback(query_options());
  QueryScope scope(*this, active_query_ ? *active_query_ : fallback);
  try {
    auto &context = *active_query_;
    context.bounds(depth, limit);
    TraversalResult out;
    // References into the cache survive rehash and are used by all predecessor chains.
    struct Resolved { Result<GraphObject> record; size_t copy_charge; };
    struct Step { const Resolved *node; const Resolved *edge; size_t parent; size_t depth; };
    std::unordered_map<std::string, Resolved> cache;
    std::unordered_set<std::string> seen;
    std::vector<Step> steps;
    auto resolve = [&](const std::string &key) -> const Resolved & {
      if (auto found = cache.find(key); found != cache.end()) return found->second;
      context.reserve_work(key.size()*2 + sizeof(Resolved)*2 + 128);
      auto charge = hot_->query_record_charge(key);
      auto record = get_by_key(key);
      return cache.emplace(key, Resolved{std::move(record), charge}).first->second;
    };
    auto resolve_node = [&](const std::string &id) -> const Resolved & {
      context.reserve_work(id.size()*2 + 32);
      return resolve(object_key(ObjectKind::Node, id));
    };
    auto copy_object = [&](const Resolved &record, std::vector<GraphObject> &destination) {
      context.reserve_work(record.copy_charge + sizeof(GraphObject)*2);
      destination.push_back(record.record.value);
    };
    if (depth != 0 && limit != 0) {
      const auto &start = resolve_node(from);
      if (!start.record && start.record.status.code != ErrorCode::notFound)
        return {start.record.status, {}};
      if (start.record) {
        context.reserve_work(from.size()*2 + sizeof(Step)*2 + 128);
        steps.push_back({&start, nullptr, 0, 0});
        seen.insert(from);
      }
    }
    for (size_t current = 0; current < steps.size() && out.nodes.size() < limit; ++current) {
      context.enforce();
      // Copy these scalars before growing steps (its storage may move).
      const auto *node = steps[current].node;
      const auto level = steps[current].depth;
      if (level >= depth) continue;
      std::string after;
      while (out.nodes.size() < limit) {
        auto key = hot_->next_query_key(inbound ? HotStore::QueryIndex::Inbound : HotStore::QueryIndex::Outbound,
                                        node->record.value.id, type, {}, after, context);
        if (!key) break;
        context.expand_edge();
        after = std::move(*key);
        const auto &edge = resolve(after);
        if (!edge.record) return {edge.record.status, {}};
        const auto &next = inbound ? edge.record.value.from : edge.record.value.to;
        if (seen.contains(next)) continue;
        const auto &destination = resolve_node(next);
        if (!destination.record) {
          if (destination.record.status.code != ErrorCode::notFound)
            return {destination.record.status, {}};
          continue; // A missing endpoint cannot be a valid intermediate path node.
        }
        context.reserve_work(next.size()*2 + sizeof(Step)*2 + 128);
        seen.insert(next);
        steps.push_back({&destination, &edge, current, level+1});
        copy_object(destination, out.nodes);
      }
    }
    if (include_paths) {
      for (size_t target = 1; target < steps.size(); ++target) {
        context.enforce();
        const auto count = steps[target].depth+1;
        context.reserve_work(count*sizeof(size_t) + sizeof(GraphPath)*2);
        std::vector<size_t> chain(count);
        auto at = target;
        for (size_t position = count; position > 0; --position) {
          chain[position-1] = at;
          at = steps[at].parent;
        }
        GraphPath path;
        for (size_t position = 0; position < count; ++position) {
          const auto &step = steps[chain[position]];
          copy_object(*step.node, path.nodes);
          if (position != 0) copy_object(*step.edge, path.edges);
        }
        out.paths.push_back(std::move(path));
      }
    }
    if (measure_output) {
      // Native results obey the same full-object envelope size without allocating JSON.
      QueryJson counter(context, true);
      counter.traversal(out);
    }
    return {Status::OK(), std::move(out)};
  } catch (const QueryFailure &e) { return {e.status, {}}; }
}

Status GraphEngine::create_node_property_index_unlocked(std::string label,
                                                        std::string property_name) {
  try {
    validate_identity(label);
    validate_identity(property_name);
  } catch (const std::exception &e) {
    return Status::Error(e.what(), ErrorCode::invalidArgument);
  }
  // Build only the requested property's postings on the transaction's staged root.
  return hot_->create_node_property_index(label,property_name,disk_.get());
}

bool GraphEngine::has_node_property_index_unlocked(const std::string &label,
                                                   const std::string &property_name) const {
  return hot_->has_node_property_index(label, property_name);
}

Status GraphEngine::rebuild_indexes_unlocked() {
  QueryContext fallback(query_options());
  auto *context=active_query_?active_query_:&fallback;
  try {
    context->reserve_work(hot_->query_clone_charge());
    auto staged=hot_->clone(context);
    auto status=staged->rebuild_indexes(disk_.get());
    if(!status.ok)return status;
    status=check_state_budget(*staged);
    if(!status.ok)return status;
    context->enforce();
    hot_->swap_state(*staged);
  } catch(const QueryFailure &e) {return e.status;}
    catch(const std::bad_alloc &) {return Status::Error("index allocation failed",ErrorCode::limitExceeded);}
  return Status::OK();
}

size_t GraphEngine::sweep_once_unlocked() {
  QueryContext ctx;
  auto trimmed = trim_unlocked(0, ctx, true);
  return trimmed ? trimmed.value.evicted_count : 0;
}

QueryOptions GraphEngine::query_options() const {
  auto options=config_.query_options;
  options.working_bytes=std::min(options.working_bytes,config_.memory.query_bytes);
  options.result_bytes=std::min(options.result_bytes,config_.memory.result_bytes);
  return options;
}
QueryResult GraphEngine::execute_dsl(std::string query, const PropertyMap &parameters) {
  QueryContext context(query_options());
  return execute_dsl(std::move(query), parameters, context);
}
QueryResult GraphEngine::execute_dsl(std::string query, const PropertyMap &parameters, QueryContext &context) {
  if (reentrant())
    return {false,
            {},
            "snapshot reentrancy is not allowed",
            Status::Error("snapshot reentrancy is not allowed", ErrorCode::conflict)};
  try {
    context.tighten(query_options());
    auto lock = acquire(&context);
    QueryScope scope(*this,context);
    return execute_ngql_unlocked(*this, std::move(query), parameters);
  } catch(const QueryFailure &e) {return {false,{},e.what(),e.status};}
}

thread_local std::vector<const GraphEngine *> GraphEngine::snapshot_owners_;
std::unique_lock<std::timed_mutex> GraphEngine::acquire(QueryContext *context) const {
  if (reentrant())
    throw std::invalid_argument("snapshot reentrancy is not allowed");
  std::unique_lock<std::timed_mutex> lock(operation_mu_, std::defer_lock);
  if (!context) { lock.lock(); return lock; }
  // Apply database ceilings before queueing, including snapshot acquisition.
  context->tighten(query_options());
  do { context->enforce(); } while (!lock.try_lock_for(std::chrono::milliseconds(2)));
  context->enforce();
  return lock;
}
GraphEngine::ReadSnapshot::ReadSnapshot(GraphEngine &engine, QueryContext *context)
    : engine_(engine), owned_context_(engine.query_options()), context_(context ? *context : owned_context_),
      lock_(engine.acquire(&context_)), previous_(engine.active_query_), previous_read_only_(context_.read_only) {
  context_.tighten(engine.query_options());
  snapshot_owners_.push_back(&engine);
  engine.active_query_=&context_;context_.read_only=true;
}
GraphEngine::ReadSnapshot::~ReadSnapshot() {
  engine_.active_query_=previous_;context_.read_only=previous_read_only_;
  std::erase(snapshot_owners_, &engine_);
}
QueryResult GraphEngine::ReadSnapshot::execute_dsl(std::string query, const PropertyMap &parameters) {
  try {context_.enforce();return execute_ngql_unlocked(engine_,std::move(query),parameters);}
  catch(const QueryFailure &e) {return {false,{},e.what(),e.status};}
}
Status GraphEngine::recover() {
  auto lock = acquire();
  return recover_unlocked();
}
Status GraphEngine::checkpoint() {
  if (reentrant())
    return Status::Error("write inside snapshot", ErrorCode::conflict);
  auto lock = acquire();
  return checkpoint_unlocked();
}
Status GraphEngine::checkpoint(QueryContext &context) {
  if (reentrant())
    return Status::Error("write inside snapshot", ErrorCode::conflict);
  auto lock = acquire(&context);
  // No QueryScope: this shares the synchronous/lifecycle maintenance policy.
  // Once admitted, finish the durability protocol without a late cancellation check.
  return checkpoint_unlocked();
}
size_t GraphEngine::sweep_once() {
  auto lock = acquire();
  return sweep_once_unlocked();
}
Result<GraphObject> GraphEngine::get_node(const std::string &id) {
  auto lock = acquire();
  return get_node_unlocked(id);
}
Result<GraphObject> GraphEngine::get_edge(const std::string &from, const std::string &type,
                                          const std::string &to) {
  auto lock = acquire();
  return get_edge_unlocked(from, type, to);
}
Result<std::vector<GraphObject>> GraphEngine::find_nodes(std::string label, std::string key,
                                                         Property value, size_t limit,
                                                         std::string op) {
  auto lock = acquire();
  return find_nodes_unlocked(std::move(label), std::move(key), std::move(value), limit,
                             std::move(op));
}
Result<std::vector<GraphObject>> GraphEngine::walk_out(std::string from, std::string type,
                                                       size_t depth, size_t limit) {
  auto lock = acquire();
  return walk_out_unlocked(std::move(from), std::move(type), depth, limit);
}
Result<std::vector<GraphObject>> GraphEngine::walk_in(std::string to, std::string type,
                                                      size_t depth, size_t limit) {
  auto lock = acquire();
  return walk_in_unlocked(std::move(to), std::move(type), depth, limit);
}
Result<TraversalResult> GraphEngine::traverse_out(std::string from, std::string type,
                                                  size_t depth, size_t limit, bool include_paths) {
  auto lock = acquire();
  return traverse_unlocked(from, type, depth, limit, false, include_paths, true);
}
Result<TraversalResult> GraphEngine::traverse_in(std::string to, std::string type,
                                                 size_t depth, size_t limit, bool include_paths) {
  auto lock = acquire();
  return traverse_unlocked(to, type, depth, limit, true, include_paths, true);
}
bool GraphEngine::has_node_property_index(const std::string &label, const std::string &prop) const {
  auto lock = acquire();
  return has_node_property_index_unlocked(label, prop);
}
Status GraphEngine::rebuild_indexes() {
  if (reentrant())
    return Status::Error("maintenance inside snapshot", ErrorCode::conflict);
  auto lock = acquire();
  return rebuild_indexes_unlocked();
}
Status GraphEngine::upsert_node(std::string label, std::string id, PropertyMap props) {
  QueryContext ctx;
  return commit_batch({new_transaction_id(),
                       {UpsertNode{std::move(label), std::move(id), std::move(props)}}},
                      ctx)
      .status;
}
Status GraphEngine::upsert_edge(std::string type, std::string from, std::string to,
                                PropertyMap props) {
  QueryContext ctx;
  return commit_batch(
             {new_transaction_id(),
              {UpsertEdge{std::move(type), std::move(from), std::move(to), std::move(props)}}},
             ctx)
      .status;
}
Status GraphEngine::delete_node(std::string id) {
  QueryContext ctx;
  return commit_batch({new_transaction_id(), {DeleteNode{std::move(id)}}}, ctx).status;
}
Status GraphEngine::delete_edge(std::string from, std::string type, std::string to) {
  QueryContext ctx;
  return commit_batch(
             {new_transaction_id(), {DeleteEdge{std::move(from), std::move(type), std::move(to)}}},
             ctx)
      .status;
}
Status GraphEngine::create_node_property_index(std::string label, std::string prop) {
  QueryContext ctx;
  return commit_batch({new_transaction_id(), {CreateIndex{std::move(label), std::move(prop)}}}, ctx)
      .status;
}
namespace {
Status validate_batch(const TransactionBatch &batch) {
  try {
    parse_uuid(batch.transaction_id);
    if (batch.mutations.empty() || batch.mutations.size() > kMaxBatchStatements)
      return Status::Error("batch requires 1..1000 mutations", ErrorCode::limitExceeded);
    size_t bytes = 0;
    for (const auto &m : batch.mutations) {
      auto encoded = encode_mutation(m);
      if (encoded.size() > kMaxBatchBytes - bytes)
        return Status::Error("batch exceeds 16 MiB", ErrorCode::limitExceeded);
      bytes += encoded.size();
    }
    return Status::OK();
  } catch (const std::exception &e) {
    return Status::Error(e.what(), ErrorCode::invalidArgument);
  }
}
} // namespace
Status GraphEngine::apply_mutation(const Mutation &mutation) {
  return std::visit(
      [&](const auto &m) -> Status {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, UpsertNode>)
          return upsert_node_unlocked(m.label, m.id, m.properties, m.modified_ms);
        else if constexpr (std::is_same_v<T, UpsertEdge>)
          return upsert_edge_unlocked(m.type, m.from, m.to, m.properties, m.modified_ms);
        else if constexpr (std::is_same_v<T, DeleteNode>)
          return delete_node_unlocked(m.id);
        else if constexpr (std::is_same_v<T, DeleteEdge>)
          return delete_edge_unlocked(m.from, m.type, m.to);
        else
          return create_node_property_index_unlocked(m.label, m.property);
      },
      mutation);
}
Result<CommitReceipt> GraphEngine::publish(const TransactionBatch &batch, HotStore &staged,
                                           QueryContext &context) {
  auto checked = check_state_budget(staged);
  if (!checked.ok)
    return {checked, {}};
  checked = context.check();
  if (!checked.ok)
    return {checked, {}};
  Result<CommitReceipt> receipt{
      Status::OK(), {batch.transaction_id, committed_lsn_ + batch.mutations.size() + 2}};
  if (wal_) {
    receipt = wal_->append_transaction(batch);
    if (!receipt) {
      if (!wal_->writable()) {
        write_fenced_ = true;
        checkpoint_on_destroy_ = false;
      }
      return receipt;
    }
  }
  config_.file_io->fault_point("transaction.before_publication");
  hot_->swap_state(staged);
  committed_lsn_ = receipt.value.committed_lsn;
  config_.file_io->fault_point("transaction.after_publication");
  return receipt;
}
Result<CommitReceipt> GraphEngine::commit_batch(const TransactionBatch &batch,
                                                QueryContext &context) {
  if (reentrant())
    return {Status::Error("write inside snapshot", ErrorCode::conflict), {}};
  try {
    context.tighten(query_options());
    auto lock=acquire(&context);
    if(write_fenced_)return {Status::Error("database is fenced",ErrorCode::closed),{}};
    auto check = context.check();
    if (!check.ok)
      return {check, {}};
    if(batch.mutations.size()>context.options().max_statements)
      return {Status::Error("batch statement budget exceeded",ErrorCode::limitExceeded),{}};
    size_t bytes=0;
    for(const auto &mutation:batch.mutations) {
      size_t charge=64;
      std::visit([&](const auto &m) {
        using T=std::decay_t<decltype(m)>;
        if constexpr(std::is_same_v<T,UpsertNode>)charge+=m.label.size()+m.id.size();
        if constexpr(std::is_same_v<T,UpsertEdge> || std::is_same_v<T,DeleteEdge>)charge+=m.type.size()+m.from.size()+m.to.size();
        if constexpr(std::is_same_v<T,DeleteNode>)charge+=m.id.size();
        if constexpr(std::is_same_v<T,CreateIndex>)charge+=m.label.size()+m.property.size();
        if constexpr(std::is_same_v<T,UpsertNode> || std::is_same_v<T,UpsertEdge>)
          for(const auto &[key,value]:m.properties) {context.enforce();charge+=key.size()+16;if(auto text=std::get_if<std::string>(&value))charge+=text->size();}
      },mutation);
      if(charge>context.options().batch_bytes-bytes)
        return {Status::Error("batch byte budget exceeded",ErrorCode::limitExceeded),{}};
      bytes+=charge;context.reserve_work(charge*12+4096);
    }
    context.reserve_work(hot_->query_clone_charge());
    check = validate_batch(batch);
    if (!check.ok)
      return {check, {}};
    check = check_transaction_workspace(batch);
    if (!check.ok)
      return {check, {}};
    config_.file_io->fault_point("transaction.before_stage");
    auto prepared = stamp_modifications(batch, config_.wall_clock());
    auto staged = hot_->clone(&context);
    auto config = config_;
    config.checkpoint_on_destroy = false;
    GraphEngine view(staged, disk_, config);
    view.active_query_=&context;
    for (const auto &mutation : prepared.mutations) {
      check = context.check();
      if (!check.ok)
        return {check, {}};
      check = view.apply_mutation(mutation);
      if (!check.ok)
        return {check, {}};
    }
    return publish(prepared, *staged, context);
  } catch(const QueryFailure &e) {return {e.status,{}};}
  catch (const std::bad_alloc &) {
    return {Status::Error("transaction allocation failed", ErrorCode::limitExceeded), {}};
  }
}
QueryResult GraphEngine::run_program(const TransactionBatch &batch,
                                     const std::function<QueryResult(GraphEngine &)> &run) {
  if (batch.mutations.empty())
    return run(*this);
  if (write_fenced_)
    return {
        false, {}, "database is fenced", Status::Error("database is fenced", ErrorCode::closed)};
  auto checked = validate_batch(batch);
  if (!checked.ok)
    return {false, {}, checked.message, checked};
  checked = check_transaction_workspace(batch);
  if (!checked.ok)
    return {false, {}, checked.message, checked};
  if(active_query_) {
    active_query_->reserve_work(hot_->query_clone_charge());
  }
  config_.file_io->fault_point("transaction.before_stage");
  auto timestamp = config_.wall_clock();
  auto prepared = stamp_modifications(batch, timestamp);
  auto staged = hot_->clone(active_query_);
  auto config = config_;
  config.checkpoint_on_destroy = false;
  config.wall_clock = [timestamp] { return timestamp; };
  GraphEngine view(staged, disk_, config);
  view.active_query_=active_query_;
  auto result = run(view);
  if (!result.ok)
    return result;
  QueryContext fallback(query_options());
  auto committed = publish(prepared, *staged, active_query_ ? *active_query_ : fallback);
  if (!committed)
    return {false, {}, committed.status.message, committed.status};
  result.receipt.emplace(std::move(committed.value));
  return result;
}

} // namespace graphdb

namespace graphdb {
QueryResult GraphEngine::inspect(unsigned kind, const std::string &cursor, size_t limit, QueryContext &context) {
  try {
    auto lock = acquire(&context);
    QueryScope scope(*this, context);
    context.tighten(query_options());
    if (kind > 2 || limit == 0 || limit > 100)
      throw QueryFailure(Status::Error("inspection kind or page limit is invalid", ErrorCode::invalidArgument));
    context.bounds(0, limit);
    const auto revision = std::to_string(committed_lsn_);
    const auto prefix = inspection_instance_ + ":" + revision + ":" + std::to_string(kind) + ":";
    std::string after;
    if (!cursor.empty()) {
      const auto instance_end = cursor.find(':');
      const auto colon = instance_end == std::string::npos ? std::string::npos : cursor.find(':', instance_end+1);
      if (instance_end == std::string::npos || instance_end == 0 || colon == std::string::npos ||
          colon == instance_end+1 || cursor.size() <= colon+3 ||
          cursor[colon+1] != char('0'+kind) || cursor[colon+2] != ':' ||
          cursor.substr(instance_end+1,colon-instance_end-1).find_first_not_of("0123456789") != std::string::npos)
        throw QueryFailure(Status::Error("invalid inspection cursor", ErrorCode::invalidArgument));
      if (cursor.substr(0,instance_end) != inspection_instance_ ||
          cursor.substr(instance_end+1,colon-instance_end-1) != revision)
        throw QueryFailure(Status::Error("database instance or revision changed; restart inspection", ErrorCode::conflict));
      after = cursor.substr(colon+3);
    }
    auto keys = hot_->inspection_keys(kind, after, limit+1, context);
    const bool more = keys.size() > limit;
    if (more) keys.resize(limit);
    QueryJson json(context);
    json.append("{\"kind\":");json.quoted(kind==0 ? "nodes" : kind==1 ? "edges" : "indexes");
    json.append(",\"revision\":");json.quoted(revision);
    json.append(",\"items\":[");bool first=true;
    for (const auto &key : keys) {
      if (!first) json.append(","); first=false;
      if (kind == 2) {
        // Declared index keys use the existing length-prefixed label/property encoding.
        auto colon=key.find(':');auto length=std::stoull(key.substr(0,colon));
        auto next=colon+1+length;auto second=key.find(':',next);
        json.append("{\"label\":");json.quoted(std::string_view(key).substr(colon+1,length));
        json.append(",\"property\":");json.quoted(std::string_view(key).substr(second+1));json.append("}");
      } else {
        auto record=get_by_key(key);
        if (!record) return {false,{},record.status.message,record.status};
        json.object(record.value,{});
      }
    }
    json.append("],\"nextCursor\":");
    if (more) json.quoted(prefix+keys.back()); else json.append("null");
    json.append("}");
    return {true,json.take(),{}};
  } catch (const QueryFailure &failure) { return {false,{},failure.what(),failure.status}; }
}
} // namespace graphdb
