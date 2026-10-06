#include "graphdb/GraphEngine.hpp"
#include <limits>
#include <set>
namespace graphdb {
namespace {
Status limit(const char *message) { return Status::Error(message, ErrorCode::limitExceeded); }
bool owned_name(std::string_view name, std::string_view prefix, std::string_view suffix) {
  if (!name.starts_with(prefix) || !name.ends_with(suffix) ||
      name.size() != prefix.size() + 32 + suffix.size())
    return false;
  auto id = name.substr(prefix.size(), 32);
  return std::all_of(id.begin(), id.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
} // namespace
Status GraphEngine::check_state_budget(const HotStore &state) const {
  auto usage = state.memory_usage();
  if (usage.hot_payload_bytes > config_.memory.hot_payload_bytes)
    return limit("hot payload budget exceeded; trim before writing");
  if (usage.metadata_bytes > config_.memory.metadata_bytes)
    return limit("metadata/index budget exceeded");
  return Status::OK();
}
Status GraphEngine::check_transaction_workspace(const TransactionBatch &batch) const {
  // Temporary copies and changed tree paths belong to query workspace.
  // Persistent metadata is checked on the completed staged state in publish(),
  // before any WAL append; overwrites/deletes need not grow that state.
  size_t growth = 0;
  for (const auto &mutation : batch.mutations) {
    auto encoded = encode_mutation(mutation);
    size_t charge = encoded.size() * 12 + 4096;
    std::visit(
        [&](const auto &m) {
          using T = std::decay_t<decltype(m)>;
          if constexpr (std::is_same_v<T, UpsertNode> || std::is_same_v<T, UpsertEdge>)
            charge += m.properties.size() * 512;
        },
        mutation);
    if (charge > config_.memory.query_bytes - growth)
      return limit("transaction workspace reservation exceeds budget");
    growth += charge;
  }
  if (growth > config_.memory.query_bytes)
    return limit("transaction workspace reservation exceeds budget");
  return Status::OK();
}
Result<TrimResult> GraphEngine::trim_memory(size_t target, QueryContext &context) {
  if (reentrant())
    return {Status::Error("maintenance inside snapshot", ErrorCode::conflict), {}};
  try {auto lock=acquire(&context);return trim_unlocked(target,context,false);}
  catch(const QueryFailure &e) {return {e.status,{}};}
}
Result<TrimResult> GraphEngine::trim_unlocked(size_t target, QueryContext &context, bool ttl) {
  if (write_fenced_ || (recovery_ && recovery_->fenced()))
    return {Status::Error("maintenance is fenced", ErrorCode::closed), {}};
  auto checked = context.check();
  if (!checked.ok)
    return {checked, {}};
  TrimResult result;
  try {
    auto ceiling = context.options();
    ceiling.working_bytes = std::min(ceiling.working_bytes, config_.memory.query_bytes);
    context.tighten(ceiling);
    auto usage = hot_->memory_usage(&context);
    auto remaining = usage.hot_payload_bytes;
    auto metadata = usage.metadata_bytes;
    auto now = config_.monotonic_clock();
    auto cutoff = ttl ? (config_.hot_ttl_ms > 0 &&
                            now < std::numeric_limits<int64_t>::min() + config_.hot_ttl_ms
                        ? std::numeric_limits<int64_t>::min() : now-config_.hot_ttl_ms)
                      : std::numeric_limits<int64_t>::max();
    struct Cursor { WorkReservation reservation; std::string key; };
    std::optional<Cursor> cursor;
    size_t selected = 0;
    while (selected < config_.sweep_batch_size && (ttl || remaining > target)) {
      context.enforce();
      // Leave enough space for the bounded writer's path/name storage, even at
      // the maximum group size. Release this guard just before the writer reserves.
      const auto directory_bytes = config_.database_dir.native().size();
      if (directory_bytes > SIZE_MAX-5) throw QueryFailure(limit("trim path size overflow"));
      const auto path_size = directory_bytes+5;
      if (path_size > (SIZE_MAX/64-8192)/2)
        throw QueryFailure(limit("trim path size overflow"));
      const auto maximum = std::min(size_t(64), config_.sweep_batch_size-selected);
      // Adapt container/path count as well as payload copies to small budgets.
      auto group_limit = std::min(maximum, context.working_available() /
          (sizeof(TrimCandidate)+8192+2*path_size));
      if (!group_limit) throw QueryFailure(limit("trim workspace cannot hold group overhead"));
      std::optional<TrimBatch> selected_batch;
      while (!selected_batch) {
        auto io_space = context.hold_work(4096+group_limit*(1024+2*path_size));
        try {
          selected_batch.emplace(hot_->trim_candidates(cutoff, group_limit,
              cursor?cursor->key:std::string{}, ttl?SIZE_MAX:remaining-target, context));
        } catch (const QueryFailure &e) {
          // A first record may fit with less container/path overhead. Retry down
          // to one before declaring it oversized; cancellation is never retried.
          if (e.status.code != ErrorCode::limitExceeded || group_limit == 1) throw;
          group_limit = std::max(size_t(1), group_limit/2);
        }
      }
      auto &batch = *selected_batch;
      if (batch.candidates.empty()) break;
      selected += batch.candidates.size();
      auto written = disk_->write_trim_batch(batch.candidates, context);
      if (!written) return {written.status, result};
      if (written.value.size() != batch.candidates.size())
        return {Status::Error("trim backend returned invalid descriptor count", ErrorCode::corruptData), result};
      for (size_t i=0; i<batch.candidates.size(); ++i) {
        context.enforce();
        const auto &candidate = batch.candidates[i];
        if (hot_->publish_trim(candidate, written.value[i], metadata,
                               config_.memory.metadata_bytes, context)) {
          ++result.evicted_count;
          result.evicted_bytes += candidate.record.payload_bytes;
          remaining -= candidate.record.payload_bytes;
          config_.file_io->fault_point("payload.after_residency_switch");
        }
      }
      // Cursor owns a separate lease after this group's candidate leases end.
      auto &key = batch.candidates.back().key;
      auto lease = context.hold_work(key.capacity()+64);
      cursor.reset();
      cursor.emplace(Cursor{std::move(lease), std::move(key)});
    }
    result.pinned_bytes = hot_->pinned_hot_bytes(context);
    result.unmet_bytes = remaining > target ? remaining-target : 0;
    return {Status::OK(), result};
  } catch (const QueryFailure &e) {
    return {e.status, result};
  } catch (const std::bad_alloc &) {
    return {limit("trim allocation failed"), result};
  }
}

Result<std::shared_ptr<void>> GraphEngine::pin_node(const std::string &id) {
  auto lock = acquire();
  auto loaded = get_node_unlocked(id);
  if (!loaded)
    return {loaded.status, {}};
  auto pin = hot_->pin(object_key(ObjectKind::Node, id));
  if (!pin)
    return {limit("payload cannot fit in hot budget"), {}};
  return {Status::OK(), std::move(pin)};
}
Result<GarbageResult> GraphEngine::collect_garbage(size_t max_files, QueryContext &context) {
  if(reentrant())return {Status::Error("maintenance inside snapshot",ErrorCode::conflict),{}};
  GarbageResult result;
  try {
    auto lock=acquire(&context);
    if(write_fenced_ || (recovery_ && recovery_->fenced()))return {Status::Error("maintenance is fenced",ErrorCode::closed),{}};
    context.enforce();if(!max_files)return {Status::OK(),result};
    auto ceiling=context.options();ceiling.working_bytes=std::min(ceiling.working_bytes,config_.memory.query_bytes);
    context.tighten(ceiling);
    // Reader + fallback + parser and path scratch remain available while a page
    // is retained. 1024 bytes per name covers vector growth, filename storage,
    // marks and the directory iterator's transient path on supported filesystems.
    auto scratch=context.hold_work(4096+8*config_.database_dir.native().size());
    const size_t scan_bytes=catalog_scan_workspace+4*(config_.database_dir.native().size()+256);
    const auto available=context.working_available();
    if(available<scan_bytes+1024)return {limit("GC workspace cannot hold reader and one candidate"),{}};
    max_files=std::min({max_files,size_t(4096),(available-scan_bytes)/1024});
    auto page_reservation=context.hold_work(max_files*1024);
    struct Page {size_t area=0,limit=0,processed=0,removed=0;std::vector<std::string> names;std::vector<bool> live;};
    std::array<Page,2> pages;
    size_t selected=0;
    for(size_t turn=0;turn<2 && selected<max_files;++turn){
      context.enforce();auto &page=pages[turn];page.area=(gc_area_+turn)%2;
      page.limit=std::min(max_files-selected,std::max(size_t(1),max_files/2));
      auto dir=page.area?config_.database_dir:config_.database_dir/"cold";
      auto names=config_.file_io->list(dir,gc_offsets_[page.area],page.limit);
      if(!names){if(names.status.code==ErrorCode::notFound)continue;return {names.status,result};}
      if(names.value.size()>page.limit)return {limit("directory backend exceeded GC page bound"),result};
      for(const auto &name:names.value)if(name.size()>255)return {limit("directory filename exceeds GC bound"),result};
      page.names=std::move(names.value);std::sort(page.names.begin(),page.names.end());
      page.live.resize(page.names.size(),false);selected+=page.names.size();
    }
    auto mark=[&](std::string_view name){
      for(auto &page:pages)if(!page.area){
        auto it=std::lower_bound(page.names.begin(),page.names.end(),name);
        if(it!=page.names.end() && *it==name)page.live[it-page.names.begin()]=true;
      }
    };
    hot_->visit_cold_references(mark,context);
    if(recovery_){
      const auto &root=recovery_->root();
      for(auto &page:pages)if(page.area)for(size_t i=0;i<page.names.size();++i){
        const auto &name=page.names[i];bool live=name==root.current.filename || name==root.active.filename || (root.previous && name==root.previous->filename);
        for(const auto &segment:root.sealed){context.enforce();live=live || name==segment.filename;}
        page.live[i]=live;
      }
      auto status=scan_retained_catalog(*config_.file_io,config_.database_dir/root.current.filename,root.current,root.database_id,context,mark);
      if(!status.ok)return {status,result};
      if(root.previous){status=scan_retained_catalog(*config_.file_io,config_.database_dir/root.previous->filename,*root.previous,root.database_id,context,mark);if(!status.ok)return {status,result};}
    } else {
      auto reader_reservation=context.hold_work(scan_bytes);
      auto reader=config_.file_io->open_reader(catalog_path_,64*1024*1024,16384);
      if(reader){
        const auto bytes=reader.value->size();auto status=reader.value->close();if(!status.ok)return {status,result};
        // Supported ABI bound for strict decoding + validation: each record is
        // at least 30 encoded bytes; each posting >=12; scalar >=5. Their live
        // record, tree, vector, validation and copied-string allocations fit
        // within 256 times encoded extent (plus allocator/container slack).
        static_assert(sizeof(StoredRecord)<=512 && sizeof(Property)<=64);
        if(bytes>(std::numeric_limits<size_t>::max()-4096)/256)return {limit("legacy catalog reservation overflow"),result};
        auto catalog_reservation=context.hold_work(4096+256*size_t(bytes));
        auto catalog=Catalog::load(catalog_path_,config_.file_io);if(!catalog)return {catalog.status,result};
        for(const auto &record:catalog.value.records){context.enforce();if(record.tier==StorageTier::Cold)mark(record.disk_key);}
      } else if(reader.status.code!=ErrorCode::notFound)return {reader.status,result};
    }
    context.enforce();config_.file_io->fault_point("gc.after_mark");
    // No directory offset changes or deletion occur before all marking succeeds.
    auto finish=[&](Status status)->Result<GarbageResult>{
      for(auto &page:pages)if(page.limit){
        if(page.removed){auto synced=config_.file_io->sync_directory(page.area?config_.database_dir:config_.database_dir/"cold");
          if(status.ok && !synced.ok)status=synced;
          if(synced.ok)config_.file_io->fault_point("gc.after_directory_sync");}
        // Sorting the page means a partial pass cannot safely advance its cursor.
        if(page.processed==page.names.size())gc_offsets_[page.area]=page.names.size()<page.limit?0:gc_offsets_[page.area]+page.processed-page.removed;
      }
      gc_area_=(gc_area_+1)%2;return {status,result};
    };
    for(auto &page:pages){
      auto dir=page.area?config_.database_dir:config_.database_dir/"cold";
      for(size_t i=0;i<page.names.size();++i){
        auto status=context.check();if(!status.ok)return finish(status);
        const auto &name=page.names[i];++result.inspected;++page.processed;
        bool owned=page.area?(recovery_ && (owned_name(name,"catalog-",".db") || owned_name(name,"wal-",".log") || owned_name(name,"catalog-",".db.tmp") || owned_name(name,"wal-",".log.tmp") || name=="CURRENT.tmp")):owned_name(name,"payload-",".rec");
        if(!owned || page.live[i])continue;
        config_.file_io->fault_point("gc.before_remove");status=config_.file_io->remove(dir/name);
        if(!status.ok){--page.processed;return finish(status);}
        ++page.removed;++result.removed;config_.file_io->fault_point("gc.after_remove");
      }
    }
    return finish(Status::OK());
  } catch(const QueryFailure &error){return {error.status,result};}
}
} // namespace graphdb
