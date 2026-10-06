#include "graphdb/CGraphDB.h"
#include "CGraphDBInternal.hpp"
#include "graphdb/DatabaseLock.hpp"
#include "graphdb/DslParser.hpp"
#include "graphdb/GraphEngine.hpp"
#include "graphdb/MaintenanceOperations.hpp"
#include "graphdb/ValueCodec.hpp"

#include <cstring>
#include <charconv>
#include <filesystem>
#include <memory>
#include <string>

using namespace graphdb;
struct GraphDBRequest {
  std::atomic<size_t> references{1};
  std::shared_ptr<RequestState> state;
  explicit GraphDBRequest(int64_t timeout) : state(std::make_shared<RequestState>(timeout)) {}
};
struct GraphDBHandle {
  std::atomic<size_t> references{1};
  enum class Phase { open, suspending, suspended, closing, closed };
  std::mutex lifecycle;
  std::condition_variable drained;
  Phase phase{Phase::open};
  bool transitioning{false};
  Status close_status, suspend_status;
  QueryOptions options;
  const GraphEngine *identity{nullptr};
  std::vector<std::shared_ptr<RequestState>> active;
  // Declared first, released last: cleanup still owns the directory.
  std::unique_ptr<DatabaseLock> lock;
  std::shared_ptr<MemoryHotStore> hot;
  std::shared_ptr<FileDiskStore> disk;
  std::shared_ptr<WalManager> wal;
  std::unique_ptr<GraphEngine> engine;
};
struct GraphDBSnapshot {
  GraphEngine::ReadSnapshot *view;
  QueryContext *context;
  std::thread::id owner;
};
namespace {
GraphDBString execute_query_buffered(GraphDBHandle *, const char *, unsigned long long,
    const char *, unsigned long long, const GraphDBQueryOptions *, bool, GraphDBRequest * = nullptr, GraphDBSnapshot * = nullptr);
struct HandleReference {
  GraphDBHandle *handle;
  explicit HandleReference(GraphDBHandle *h) : handle(h) { graphdb_retain(handle); }
  ~HandleReference() {graphdb_release(handle);}
};
struct OperationLease {
  HandleReference reference;
  std::shared_ptr<RequestState> request;
  explicit OperationLease(GraphDBHandle *h, GraphDBRequest *r = nullptr, bool pressure = false)
      : reference(h), request(r ? r->state : std::make_shared<RequestState>(h->options.timeout_ms)) {
    std::lock_guard lock(h->lifecycle);
    if(h->phase!=GraphDBHandle::Phase::open && !(pressure && h->phase==GraphDBHandle::Phase::suspended))
      throw QueryFailure(Status::Error("database is not accepting operations",
        h->phase==GraphDBHandle::Phase::suspended || h->phase==GraphDBHandle::Phase::suspending ? ErrorCode::busy : ErrorCode::closed));
    if(h->engine->snapshot_on_current_thread())
      throw QueryFailure(Status::Error("database reentrancy inside snapshot",ErrorCode::conflict));
    h->active.push_back(request);
  }
  ~OperationLease() {
    auto h=reference.handle;
    { std::lock_guard lock(h->lifecycle);
      auto found=std::find(h->active.begin(),h->active.end(),request);
      if(found!=h->active.end())h->active.erase(found);
    }
    h->drained.notify_all();
  }
};
GraphDBString make_string(const std::string &s) {
  auto bytes = std::make_unique<char[]>(s.size() + 1);
  std::memcpy(bytes.get(), s.data(), s.size());
  bytes[s.size()] = 0;
  return {bytes.release(), static_cast<unsigned long long>(s.size())};
}
GraphDBString make_error(const Status &status) noexcept {
  try {
    std::string json = "{\"ok\":false,\"error\":\"" + json_escape(status.message) +
                       "\",\"code\":\"" + error_code_name(status.code) + "\",\"context\":{";
    json += "\"file\":\"" + json_escape(status.context.file) +
            "\",\"osError\":" + std::to_string(status.context.os_error);
    json += ",\"object\":\"" + json_escape(status.context.object) + "\",\"transactionId\":\"" +
            json_escape(status.context.transaction_id) + "\"";
    if(status.context.statement_index) json += ",\"statementIndex\":"+std::to_string(*status.context.statement_index);
    if(status.context.source_start) json += ",\"sourceStart\":"+std::to_string(*status.context.source_start);
    if(status.context.source_end) json += ",\"sourceEnd\":"+std::to_string(*status.context.source_end);
    json += "}}";
    return make_string(json);
  } catch (...) {
    return {nullptr, 0};
  }
}
Status exception_status() noexcept {
  try {
    throw;
  } catch (const QueryFailure &e) {
    try {return e.status;} catch(...) {}
  } catch (const std::invalid_argument &e) {
    try {
      return Status::Error(e.what(), ErrorCode::invalidArgument);
    } catch (...) {
    }
  } catch (const std::filesystem::filesystem_error &e) {
    try {
      return Status::Error(e.what(), ErrorCode::ioFailure, {e.path1().string(), e.code().value()});
    } catch (...) {
    }
  } catch (const std::exception &e) {
    try {
      return Status::Error(e.what(), ErrorCode::ioFailure);
    } catch (...) {
    }
  } catch (...) {
  }
  return {false, {}, ErrorCode::ioFailure, {}};
}
template <class F> GraphDBString boundary(F action) noexcept {
  try {
    return action();
  } catch (...) {
    return make_error(exception_status());
  }
}
} // namespace

GraphDBQueryOptions graphdb_default_query_options(void) {
  QueryOptions q;
  return {q.max_depth,q.max_results,q.max_expanded_edges,q.working_bytes,q.result_bytes,q.max_statements,q.batch_bytes,static_cast<unsigned long long>(q.timeout_ms)};
}
GraphDBOpenResult graphdb_open_result(const char *database_dir, long long hot_ttl_ms,
                                      long long sweep_interval_ms) {
  return graphdb_open_with_options_result(database_dir,hot_ttl_ms,sweep_interval_ms,nullptr);
}
GraphDBOpenResult graphdb_open_with_options_result(const char *database_dir, long long hot_ttl_ms,
                                      long long sweep_interval_ms, const GraphDBQueryOptions *options) {
  try {return graphdb::detail::open_with_io(database_dir,hot_ttl_ms,sweep_interval_ms,options,default_file_io());}
  catch(...) {return {nullptr,make_error(exception_status())};}
}
GraphDBOpenResult graphdb::detail::open_with_io(const char *database_dir,long long hot_ttl_ms,
    long long sweep_interval_ms,const GraphDBQueryOptions *options,std::shared_ptr<FileIO> io) {
  try {
    if(!io)return {nullptr,make_error(Status::Error("missing filesystem backend",ErrorCode::invalidArgument))};
    QueryOptions query_options;
    if(options) {
      const auto &q=*options;
      if(q.max_depth>query_options.max_depth || q.max_results>query_options.max_results || q.max_expanded_edges>query_options.max_expanded_edges ||
         q.working_bytes>query_options.working_bytes || q.result_bytes>query_options.result_bytes || q.max_statements>query_options.max_statements ||
         q.batch_bytes>query_options.batch_bytes || q.timeout_ms>static_cast<unsigned long long>(query_options.timeout_ms))
        return {nullptr,make_error(Status::Error("query options exceed supported ceilings",ErrorCode::invalidArgument))};
      query_options={size_t(q.max_depth),size_t(q.max_results),size_t(q.max_expanded_edges),size_t(q.working_bytes),
                     size_t(q.result_bytes),size_t(q.max_statements),size_t(q.batch_bytes),int64_t(q.timeout_ms)};
    }
    auto valid=query_options.validate();
    if(!valid.ok)return {nullptr,make_error(valid)};
    if (database_dir && strnlen(database_dir, 4097) > 4096)
      return {nullptr, make_error(Status::Error("database path exceeds 4096 bytes",
                                                ErrorCode::invalidArgument))};
    auto root =
        std::filesystem::path(database_dir && *database_dir ? database_dir : "/tmp/nova-graph");
    // Reject recognizable legacy data before creating an ownership file.
    if (!std::filesystem::exists(root / "CURRENT")) {
      for (auto name : {"catalog.db", "wal.log"}) {
        auto bytes = io->read(root / name, 64 * 1024 * 1024);
        if (bytes && bytes.value.size() >= 8) {
          ByteReader reader(bytes.value.data(), bytes.value.size());
          uint32_t magic, version;
          reader.read_u32(magic);
          reader.read_u32(version);
          if ((magic == 0x4E434154 || magic == 0x4E57414C) && version == 1)
            return {
                nullptr,
                make_error(Status::Error(
                    "legacy v1 requires explicit migrate-v1 into a new directory; preserve source",
                    ErrorCode::unsupportedVersion))};
        }
      }
    }
    auto ownership = DatabaseLock::acquire(root);
    if (!ownership)
      return {nullptr, make_error(ownership.status)};
    auto handle = std::make_unique<GraphDBHandle>();
    handle->lock = std::move(ownership.value);
    root = handle->lock->directory();
    auto recovery = Recovery::open(root, io);
    if (!recovery)
      return {nullptr, make_error(recovery.status)};
    handle->wal = recovery.value->wal();
    handle->hot = std::make_shared<MemoryHotStore>();
    handle->disk = std::make_shared<FileDiskStore>(root / "cold", io);
    EngineConfig cfg;
    cfg.database_dir = root;
    cfg.hot_ttl_ms = hot_ttl_ms > 0 ? hot_ttl_ms : 600000;
    cfg.sweep_interval_ms = sweep_interval_ms > 0 ? sweep_interval_ms : 30000;
    cfg.file_io = io;
    cfg.query_options = query_options;
    cfg.checkpoint_on_destroy = false;
    handle->options = query_options;
    handle->engine =
        std::make_unique<GraphEngine>(handle->hot, handle->disk, cfg, handle->wal, recovery.value);
    handle->identity=handle->engine.get();
    auto recovered = handle->engine->recover();
    if (!recovered.ok)
      return {nullptr, make_error(recovered)};
    auto started = handle->engine->start_tiering_worker();
    if (!started.ok)
      return {nullptr, make_error(started)};
    handle->engine->enable_close_checkpoint();
    return {handle.release(), {nullptr, 0}};
  } catch (...) {
    return {nullptr, make_error(exception_status())};
  }
}
GraphDBHandle *graphdb_open_with_config(const char *path, long long ttl, long long sweep) {
  auto result = graphdb_open_result(path, ttl, sweep);
  graphdb_string_free(result.error);
  return result.handle;
}
GraphDBHandle *graphdb_open(const char *path) {
  return graphdb_open_with_config(path, 600000, 30000);
}
GraphDBHandle *graphdb_open_memory(const char *path) { return graphdb_open(path); }
void graphdb_retain(GraphDBHandle *handle) { if(handle)handle->references.fetch_add(1,std::memory_order_relaxed); }
void graphdb_release(GraphDBHandle *handle) {
  if(handle && handle->references.fetch_sub(1,std::memory_order_acq_rel)==1) {
    try {delete handle;} catch(...) {}
  }
}
void graphdb_close(GraphDBHandle *handle) {
  if(!handle)return;
  graphdb_string_free(graphdb_close_v2(handle,nullptr));
  graphdb_release(handle);
}
GraphDBRequest *graphdb_request_create(unsigned long long timeout_ms) {
  if(!timeout_ms || timeout_ms>5000)return nullptr;
  try {return new GraphDBRequest(int64_t(timeout_ms));} catch(...) {return nullptr;}
}
void graphdb_request_retain(GraphDBRequest *r) {if(r)r->references.fetch_add(1,std::memory_order_relaxed);}
void graphdb_request_release(GraphDBRequest *r) {if(r && r->references.fetch_sub(1,std::memory_order_acq_rel)==1)delete r;}
void graphdb_request_cancel(GraphDBRequest *r) {if(r)r->state->cancelled.store(true);}
int graphdb_request_is_cancelled(GraphDBRequest *r) {return r && r->state->cancelled.load();}
int graphdb_request_is_expired(GraphDBRequest *r) {return r && std::chrono::steady_clock::now()>=r->state->deadline;}
GraphDBString graphdb_execute_query(GraphDBHandle *handle, const char *query) {
  return graphdb_execute_query_with_params(handle, query, nullptr);
}
GraphDBString graphdb_execute_query_with_params(GraphDBHandle *handle, const char *query,
                                                const char *params_json) {
  return boundary([&] {
    if (!handle || !query)
      return make_error(Status::Error("invalid handle or query", ErrorCode::invalidArgument));
    if (strnlen(query, kMaxStringBytes + 1) > kMaxStringBytes ||
        (params_json && strnlen(params_json, kMaxStringBytes + 1) > kMaxStringBytes))
      return make_error(
          Status::Error("query or parameters exceed 1 MiB", ErrorCode::limitExceeded));
    return execute_query_buffered(handle, query, std::strlen(query), params_json,
                                  params_json ? std::strlen(params_json) : 0, nullptr, false);
  });
}
GraphDBString graphdb_sweep_once(GraphDBHandle *handle) {
  return boundary([&] {
    if (!handle)
      return make_error(Status::Error("invalid handle", ErrorCode::invalidArgument));
    OperationLease lease(handle);
    return make_string("{\"ok\":true,\"moved\":" + std::to_string(handle->engine->sweep_once()) +
                       "}");
  });
}
GraphDBString graphdb_checkpoint(GraphDBHandle *handle) {
  return boundary([&] {
    if (!handle)
      return make_error(Status::Error("invalid handle", ErrorCode::invalidArgument));
    OperationLease lease(handle);
    auto status = handle->engine->checkpoint();
    return status.ok ? make_string("{\"ok\":true}") : make_error(status);
  });
}
GraphDBString graphdb_trim_memory(GraphDBHandle *handle) {
  return boundary([&] {
    if (!handle)
      return make_error(Status::Error("invalid handle", ErrorCode::invalidArgument));
    OperationLease lease(handle,nullptr,true);
    QueryContext context(handle->options); context.use_request(lease.request);
    auto result = handle->engine->trim_memory(0, context);
    if (!result)
      return make_error(result.status);
    return make_string("{\"ok\":true,\"evicted\":" + std::to_string(result.value.evicted_count) +
                       ",\"evictedBytes\":" + std::to_string(result.value.evicted_bytes) +
                       ",\"pinnedBytes\":" + std::to_string(result.value.pinned_bytes) +
                       ",\"unmetBytes\":" + std::to_string(result.value.unmet_bytes) + "}");
  });
}
GraphDBString graphdb_collect_garbage(GraphDBHandle *handle, unsigned long long max_files) {
  return boundary([&] {
    if (!handle)
      return make_error(Status::Error("invalid handle", ErrorCode::invalidArgument));
    OperationLease lease(handle);
    QueryContext ctx(handle->options); ctx.use_request(lease.request);
    auto result =
        handle->engine->collect_garbage(static_cast<size_t>(std::min(max_files, 4096ULL)), ctx);
    if (!result)
      return make_error(result.status);
    return make_string("{\"ok\":true,\"inspected\":" + std::to_string(result.value.inspected) +
                       ",\"removed\":" + std::to_string(result.value.removed) + "}");
  });
}
GraphDBString graphdb_rebuild_indexes(GraphDBHandle *handle) {
  return boundary([&] {
    if (!handle)
      return make_error(Status::Error("invalid handle", ErrorCode::invalidArgument));
    OperationLease lease(handle);
    auto status = handle->engine->rebuild_indexes();
    return status.ok ? make_string("{\"ok\":true}") : make_error(status);
  });
}
void graphdb_string_free(GraphDBString value) {
  try {
    delete[] value.data;
  } catch (...) {
  }
}

GraphDBString graphdb_maintenance(const char *command, const char *source,
                                  const char *destination) {
  return boundary([&] {
    if (!command || !source || strnlen(command, 32) >= 32 || strnlen(source, 4097) > 4096 ||
        (destination && strnlen(destination, 4097) > 4096))
      return make_error(
          Status::Error("invalid maintenance command/path", ErrorCode::invalidArgument));
    auto result = maintain(command, source, destination ? destination : "");
    return result ? make_string(maintenance_json(result.value)) : make_error(result.status);
  });
}
GraphDBString graphdb_backup(GraphDBHandle *handle, const char *destination) {
  return boundary([&] {
    if (!handle || !destination || strnlen(destination, 4097) > 4096)
      return make_error(Status::Error("invalid backup handle/path", ErrorCode::invalidArgument));
    OperationLease lease(handle);
    auto status = handle->engine->backup(destination);
    return status.ok ? make_string("{\"ok\":true}") : make_error(status);
  });
}

namespace {
GraphDBString versioned_error(const Status &status) noexcept {
  try {
    auto legacy = make_error(status);
    struct Free { GraphDBString s; ~Free(){ graphdb_string_free(s); } } free{legacy};
    if (!legacy.data) return {nullptr,0};
    std::string old(legacy.data,legacy.len);
    // Reuse the canonical status/context serializer, without parsing user data.
    auto context = old.find(",\"context\":");
    return make_string(std::string("{\"schemaVersion\":2,\"ok\":false,\"error\":{\"code\":\"") +
      error_code_name(status.code) + "\",\"message\":\"" + json_escape(status.message) + "\"" +
      old.substr(context, old.size()-context-1) + "}}");
  } catch (...) { return {nullptr,0}; }
}
QueryOptions converted_options(const GraphDBQueryOptions *source, QueryOptions fallback) {
  if (!source) return fallback;
  const auto &q=*source;
  const QueryOptions maximum;
  if(q.max_depth>maximum.max_depth || q.max_results>maximum.max_results || q.max_expanded_edges>maximum.max_expanded_edges ||
     q.working_bytes>maximum.working_bytes || q.result_bytes>maximum.result_bytes || q.max_statements>maximum.max_statements ||
     q.batch_bytes>maximum.batch_bytes || q.timeout_ms>static_cast<unsigned long long>(maximum.timeout_ms))
    throw QueryFailure(Status::Error("query options exceed supported ceilings",ErrorCode::invalidArgument));
  QueryOptions result{size_t(q.max_depth),size_t(q.max_results),size_t(q.max_expanded_edges),size_t(q.working_bytes),
    size_t(q.result_bytes),size_t(q.max_statements),size_t(q.batch_bytes),int64_t(q.timeout_ms)};
  auto valid=result.validate(); if(!valid.ok)throw QueryFailure(valid);
  return result;
}
}
GraphDBOpenResult graphdb_open_v2(const char *path, unsigned long long length,
    long long ttl, long long sweep, const GraphDBQueryOptions *options) {
  try {
    if(!path || length==0 || length>4096 || std::memchr(path,0,size_t(length)))
      return {nullptr,versioned_error(Status::Error("invalid database path",ErrorCode::invalidArgument))};
    std::string name(path,size_t(length)); (void)json_escape(name);
    auto result=graphdb_open_with_options_result(name.c_str(),ttl,sweep,options);
    if(result.handle || !result.error.data)return result;
    // The open implementation already catches exceptions; retain exact context via envelope adaptation.
    struct Free { GraphDBString s; ~Free(){graphdb_string_free(s);} } free{result.error};
    std::string old(result.error.data,result.error.len);
    const auto marker=old.find(",\"code\":");
    const auto prefix=std::string_view("{\"ok\":false,\"error\":").size();
    const auto message=old.substr(prefix,marker-prefix);
    return {nullptr,make_string("{\"schemaVersion\":2,\"ok\":false,\"error\":{\"message\":"+message+
      old.substr(marker,old.size()-marker-1)+"}}")};
  } catch (...) { return {nullptr,versioned_error(exception_status())}; }
}
namespace {
GraphDBString execute_query_buffered(GraphDBHandle *handle, const char *query,
    unsigned long long query_len, const char *params, unsigned long long params_len,
    const GraphDBQueryOptions *options, bool versioned, GraphDBRequest *request, GraphDBSnapshot *snapshot) {
  const auto error = versioned ? versioned_error : make_error;
  try {
    if((!handle && !snapshot) || !query || (!params && params_len))
      return error(Status::Error("invalid handle or input buffer",ErrorCode::invalidArgument));
    if(query_len>kMaxStringBytes || params_len>kMaxStringBytes)
      return error(Status::Error("query or parameters exceed 1 MiB",ErrorCode::limitExceeded));
    std::optional<OperationLease> lease;
    std::optional<QueryContext> owned_context;
    if(snapshot) {
      if(snapshot->owner!=std::this_thread::get_id())
        return error(Status::Error("snapshot belongs to another thread",ErrorCode::conflict));
    } else {
      lease.emplace(handle,request);
      owned_context.emplace(converted_options(options,handle->options));
      owned_context->use_request(lease->request);
      owned_context->tighten(handle->engine->configured_query_options());
    }
    auto &context=snapshot ? *snapshot->context : *owned_context;
    context.tagged_values=versioned;
    // Reserve envelope/receipt capacity before any mutation. The returned C allocation
    // is separate from query working storage, capped by the result budget.
    context.reserve_result(versioned ? 256 : 32);
    std::unique_ptr<char[]> output(new char[context.options().result_bytes+1]);
    context.reserve_work(size_t(query_len)*4 + size_t(params_len)*32 + 512);
    std::string text(query,size_t(query_len)); (void)json_escape(text);
    PropertyMap bindings;
    if(params_len) {
      const std::string input(params,size_t(params_len));
      bindings=versioned ? parse_json_properties(input) : parse_property_list(input);
    }
    auto result=snapshot ? snapshot->view->execute_dsl(std::move(text),bindings)
                         : handle->engine->execute_dsl(std::move(text),bindings,context);
    if(!result.ok)return error(result.status);
    // No allocating/throwing work after commit: the buffer was allocated above,
    // and receipt strings are moved out of the durable commit result.
    size_t used=0;
    auto append=[&](std::string_view part) noexcept {std::memcpy(output.get()+used,part.data(),part.size());used+=part.size();};
    append(versioned ? "{\"schemaVersion\":2,\"ok\":true,\"data\":" : "{\"ok\":true,\"data\":");append(result.json);
    if(versioned && result.receipt) {
      append(",\"receipt\":{\"transactionId\":\"");append(result.receipt->transaction_id);
      append("\",\"committedLSN\":"); char digits[32];
      auto end=std::to_chars(digits,digits+sizeof(digits),result.receipt->committed_lsn).ptr;
      append({digits,size_t(end-digits)});append("}");
    }
    append("}");output[used]=0;
    return {output.release(),static_cast<unsigned long long>(used)};
  } catch(const QueryFailure &e) { return error(e.status); }
    catch(...) { return error(exception_status()); }
}
} // namespace
GraphDBString graphdb_execute_query_v2(GraphDBHandle *handle, const char *query,
    unsigned long long query_len, const char *params, unsigned long long params_len,
    const GraphDBQueryOptions *options) {
  return execute_query_buffered(handle,query,query_len,params,params_len,options,true);
}

GraphDBString graphdb_execute_request_v2(GraphDBHandle *handle, const char *query,
    unsigned long long query_len, const char *params, unsigned long long params_len,
    const GraphDBQueryOptions *options, GraphDBRequest *request) {
  return execute_query_buffered(handle,query,query_len,params,params_len,options,true,request);
}
namespace {
GraphDBString finish_lifecycle(GraphDBHandle *handle, GraphDBRequest *request, bool closing) {
  try {
    if(!handle)return versioned_error(Status::Error("invalid handle",ErrorCode::invalidArgument));
    HandleReference reference(handle);
    QueryContext context(handle->options);
    if(request)context.use_request(request->state);
    std::unique_lock lock(handle->lifecycle);
    if(GraphEngine::snapshot_owned_by_current_thread(handle->identity))
      return versioned_error(Status::Error("close inside snapshot",ErrorCode::conflict));
    while(handle->transitioning) {context.enforce();handle->drained.wait_for(lock,std::chrono::milliseconds(2));}
    if(handle->phase==GraphDBHandle::Phase::closed) {
      if(!closing)return versioned_error(Status::Error("database is closed",ErrorCode::closed));
      return handle->close_status.ok ? make_string("{\"schemaVersion\":2,\"ok\":true,\"data\":{}}") : versioned_error(handle->close_status);
    }
    if(!closing && handle->phase==GraphDBHandle::Phase::closing)
      return versioned_error(Status::Error("database is closing",ErrorCode::closed));
    if(!closing && handle->phase==GraphDBHandle::Phase::suspended)
      return handle->suspend_status.ok ? make_string("{\"schemaVersion\":2,\"ok\":true,\"data\":{}}") : versioned_error(handle->suspend_status);
    context.enforce();
    handle->transitioning=true;
    handle->phase=closing ? GraphDBHandle::Phase::closing : GraphDBHandle::Phase::suspending;
    for(const auto &active:handle->active)active->cancelled.store(true);
    try {
      while(!handle->active.empty()) {context.enforce();handle->drained.wait_for(lock,std::chrono::milliseconds(2));}
    } catch(...) {
      handle->transitioning=false;handle->drained.notify_all();throw;
    }
    lock.unlock();
    Status status;
    try {
      handle->engine->stop_tiering_worker();
      if(closing)handle->engine->disable_close_checkpoint();
      status=handle->engine->checkpoint();
    } catch(...) {status=exception_status();}
    // No admitted operation can still use these resources. Destruction never
    // retries a failed acknowledged close checkpoint.
    if(closing) {handle->engine.reset(); handle->wal.reset(); handle->disk.reset(); handle->hot.reset(); handle->lock.reset();}
    lock.lock();
    auto &saved=closing ? handle->close_status : handle->suspend_status;
    saved=std::move(status);
    handle->phase=closing ? GraphDBHandle::Phase::closed : GraphDBHandle::Phase::suspended;
    handle->transitioning=false;handle->drained.notify_all();
    return saved.ok ? make_string("{\"schemaVersion\":2,\"ok\":true,\"data\":{}}") : versioned_error(saved);
  } catch(const QueryFailure &e) {return versioned_error(e.status);}
    catch(...) {return versioned_error(exception_status());}
}

} // namespace
GraphDBString graphdb_close_v2(GraphDBHandle *handle, GraphDBRequest *request) {return finish_lifecycle(handle,request,true);}
GraphDBString graphdb_suspend_v2(GraphDBHandle *handle, GraphDBRequest *request) {return finish_lifecycle(handle,request,false);}
GraphDBString graphdb_resume_v2(GraphDBHandle *handle, GraphDBRequest *request) {
  try {
    if(!handle)return versioned_error(Status::Error("invalid handle",ErrorCode::invalidArgument));
    HandleReference reference(handle);QueryContext context(handle->options);
    if(request)context.use_request(request->state);
    std::unique_lock lock(handle->lifecycle);
    if(GraphEngine::snapshot_owned_by_current_thread(handle->identity))
      return versioned_error(Status::Error("resume inside snapshot",ErrorCode::conflict));
    while(handle->transitioning) {context.enforce();handle->drained.wait_for(lock,std::chrono::milliseconds(2));}
    if(handle->phase==GraphDBHandle::Phase::closed || handle->phase==GraphDBHandle::Phase::closing)
      return versioned_error(Status::Error("database is closed",ErrorCode::closed));
    context.enforce();
    auto started=handle->engine->start_tiering_worker();
    if(!started.ok)return versioned_error(started);
    handle->phase=GraphDBHandle::Phase::open;
    return make_string("{\"schemaVersion\":2,\"ok\":true,\"data\":{}}");
  } catch(const QueryFailure &e) {return versioned_error(e.status);}
    catch(...) {return versioned_error(exception_status());}
}
GraphDBString graphdb_checkpoint_v2(GraphDBHandle *handle,GraphDBRequest *request) {
  try {
    if(!handle)return versioned_error(Status::Error("invalid handle",ErrorCode::invalidArgument));
    OperationLease lease(handle,request);QueryContext context(handle->options);context.use_request(lease.request);
    auto status=handle->engine->checkpoint(context);
    if(!status.ok)return versioned_error(status);
    return make_string("{\"schemaVersion\":2,\"ok\":true,\"data\":{}}");
  } catch(const QueryFailure &e) {return versioned_error(e.status);}
    catch(...) {return versioned_error(exception_status());}
}
GraphDBString graphdb_trim_memory_v2(GraphDBHandle *handle,unsigned long long target,GraphDBRequest *request) {
  try {
    if(!handle)return versioned_error(Status::Error("invalid handle",ErrorCode::invalidArgument));
    OperationLease lease(handle,request,true);QueryContext context(handle->options);context.use_request(lease.request);
    const auto result=handle->engine->trim_memory(size_t(target),context);
    if(!result)return versioned_error(result.status);
    return make_string("{\"schemaVersion\":2,\"ok\":true,\"data\":{\"evicted\":"+std::to_string(result.value.evicted_count)+
      ",\"evictedBytes\":"+std::to_string(result.value.evicted_bytes)+",\"pinnedBytes\":"+std::to_string(result.value.pinned_bytes)+
      ",\"unmetBytes\":"+std::to_string(result.value.unmet_bytes)+"}}");
  } catch(const QueryFailure &e) {return versioned_error(e.status);}
    catch(...) {return versioned_error(exception_status());}
}
GraphDBString graphdb_read_snapshot_v2(GraphDBHandle *handle,const GraphDBQueryOptions *options,
    GraphDBRequest *request,GraphDBSnapshotBody body,void *user) {
  try {
    if(!handle || !body)return versioned_error(Status::Error("invalid snapshot arguments",ErrorCode::invalidArgument));
    OperationLease lease(handle,request);QueryContext context(converted_options(options,handle->options));
    context.use_request(lease.request);context.tagged_values=true;
    auto view=handle->engine->read_snapshot(context);
    GraphDBSnapshot snapshot{&view,&context,std::this_thread::get_id()};
    body(&snapshot,user);context.enforce();
    return make_string("{\"schemaVersion\":2,\"ok\":true,\"data\":{}}");
  } catch(const QueryFailure &e) {return versioned_error(e.status);}
    catch(...) {return versioned_error(exception_status());}
}
GraphDBString graphdb_snapshot_query_v2(GraphDBSnapshot *snapshot,const char *query,
    unsigned long long query_len,const char *params,unsigned long long params_len) {
  if(!snapshot)return versioned_error(Status::Error("invalid snapshot",ErrorCode::invalidArgument));
  return execute_query_buffered(nullptr,query,query_len,params,params_len,nullptr,true,nullptr,snapshot);
}

GraphDBString graphdb_inspect_v2(GraphDBHandle *handle, unsigned int kind,
    const char *cursor, unsigned long long cursor_len, unsigned int limit,
    const GraphDBQueryOptions *options, GraphDBRequest *request) {
  try {
    if (!handle || (!cursor && cursor_len))
      return versioned_error(Status::Error("invalid handle or cursor",ErrorCode::invalidArgument));
    if (cursor_len > kMaxStringBytes)
      return versioned_error(Status::Error("cursor exceeds 1 MiB",ErrorCode::limitExceeded));
    OperationLease lease(handle,request);
    QueryContext context(converted_options(options,handle->options));context.use_request(lease.request);
    context.tighten(handle->engine->configured_query_options());context.tagged_values=true;
    context.reserve_work(size_t(cursor_len)*8+256);context.reserve_result(64);
    std::string text(cursor ? cursor : "",size_t(cursor_len));(void)json_escape(text);
    auto result=handle->engine->inspect(kind,text,limit,context);
    if (!result.ok) return versioned_error(result.status);
    return make_string("{\"schemaVersion\":2,\"ok\":true,\"data\":"+result.json+"}");
  } catch (const QueryFailure &failure) { return versioned_error(failure.status); }
    catch (...) { return versioned_error(exception_status()); }
}
