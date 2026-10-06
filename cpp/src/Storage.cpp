#include "graphdb/Storage.hpp"
#include "graphdb/BinaryEncoding.hpp"
#include "graphdb/Transaction.hpp"
#include "graphdb/ValueCodec.hpp"
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace graphdb {

namespace {

void erase_from_index(PersistentMap<PersistentSet> &index,
                      const std::string &object_key, QueryContext *context) {
  for (const auto &[key, existing] : index) {
    if(context)context->enforce();
    if(!existing.count(object_key))continue;
    auto posts=existing;posts.erase(object_key,context);
    if(posts.empty())index.erase(key,context);
    else index.set(key,std::move(posts),context);
  }
}
size_t property_key_charge(const std::string &label,const std::string &property,const Property &value) {
  // JSON escaping, equality tagging and tuple construction can hold several
  // encoded copies of the indexed scalar. Unrelated properties are not copied.
  auto text=std::get_if<std::string>(&value);
  return 512+2*(label.size()+property.size())+(text?24*text->size():128);
}
size_t index_scratch_charge(const StoredRecord &record,bool decoding) {
  if(decoding)return 512+record.payload_bytes*24;
  const auto &o=record.object;
  return 512+4*(o.id.size()+o.label_or_type.size()+o.from.size()+o.to.size());
}
void add_posting(PersistentMap<PersistentSet> &index,const std::string &bucket,
                 const std::string &key,QueryContext *context) {
  auto found=index.find(bucket);
  auto posts=found==index.end()?PersistentSet{}:found->second;
  if(posts.count(key))return;
  posts.insert(key,context);index.set(bucket,std::move(posts),context);
}
} // namespace

std::string MemoryHotStore::property_index_key(const std::string &label,
                                               const std::string &property_name,
                                               const Property &value) {
  auto encoded = property_equality_key(value);
  return tuple_key({label, property_name, encoded});
}

std::string MemoryHotStore::adjacency_index_key(const std::string &node, const std::string &type) {
  return tuple_key({node, type});
}

std::string MemoryHotStore::declared_index_key(const std::string &label,
                                               const std::string &property_name) {
  return tuple_key({label, property_name});
}

std::vector<std::string> MemoryHotStore::sorted_limited(const PersistentSet &keys,
                                                        size_t limit) {
  std::vector<std::string> out;
  out.reserve(limit ? std::min(limit,keys.size()) : keys.size());
  for (const auto &key : keys) {
    if(limit && out.size()==limit) break;
    out.push_back(key);
  }
  return out;
}

void MemoryHotStore::remove_from_indexes_by_key_locked(const std::string &key) {
  erase_from_index(node_label_index_, key, workspace_);
  erase_from_index(node_property_index_, key, workspace_);
  erase_from_index(outbound_edge_index_, key, workspace_);
  erase_from_index(inbound_edge_index_, key, workspace_);
}

void MemoryHotStore::index_object_locked(const std::string &key, const GraphObject &object) {
  if (object.kind == ObjectKind::Node) {
    add_posting(node_label_index_,object.label_or_type,key,workspace_);
    for (const auto &[property_name, value] : object.properties) {
      auto declaration_scratch=workspace_?workspace_->hold_work(128+2*(object.label_or_type.size()+property_name.size())):WorkReservation{};
      if (!declared_node_property_indexes_.count(
              declared_index_key(object.label_or_type, property_name)))
        continue;
      auto scratch=workspace_?workspace_->hold_work(property_key_charge(object.label_or_type,property_name,value)):WorkReservation{};
      add_posting(node_property_index_,property_index_key(object.label_or_type,property_name,value),key,workspace_);
    }
    return;
  }

  if (object.kind == ObjectKind::Edge) {
    add_posting(outbound_edge_index_,adjacency_index_key(object.from,object.label_or_type),key,workspace_);
    add_posting(outbound_edge_index_,adjacency_index_key(object.from,""),key,workspace_);
    add_posting(inbound_edge_index_,adjacency_index_key(object.to,object.label_or_type),key,workspace_);
    add_posting(inbound_edge_index_,adjacency_index_key(object.to,""),key,workspace_);
  }
}

Status MemoryHotStore::put_hot(const std::string &key, GraphObject object) {
  std::unique_lock lock(mu_);
  if (object.last_read_ms == 0)
    object.last_read_ms = object.last_modified_ms;
  auto it = map_.find(key);
  if (it != map_.end()) {
    if (it->second.object.version == UINT64_MAX)
      return Status::Error("logical version exhausted", ErrorCode::limitExceeded);
    object.version = it->second.object.version + 1;
    remove_from_indexes_by_key_locked(key);
  } else {
    object.version = 1;
  }
  index_object_locked(key, object);
  auto bytes = encoded_object_bytes(object);
  StoredRecord record{std::move(object), StorageTier::Hot, {}, bytes, 0, 0, now_millis(), {}};
  put_record_locked(key,std::move(record));
  return Status::OK();
}

std::optional<StoredRecord> MemoryHotStore::get(const std::string &key, bool properties) {
  std::shared_lock lock(mu_);
  auto it = map_.find(key);
  if (it == map_.end())
    return std::nullopt;
  if (properties)
    return it->second;
  const auto &source = it->second;
  StoredRecord out;
  out.object.kind = source.object.kind;
  out.object.id = source.object.id;
  out.object.label_or_type = source.object.label_or_type;
  out.object.from = source.object.from;
  out.object.to = source.object.to;
  out.object.version = source.object.version;
  out.object.last_read_ms = source.object.last_read_ms;
  out.object.last_modified_ms = source.object.last_modified_ms;
  out.tier = source.tier;
  out.disk_key = source.disk_key;
  out.payload_bytes = source.payload_bytes;
  out.payload_crc = source.payload_crc;
  out.activity_version = source.activity_version;
  out.activity_tick = source.activity_tick;
  out.pins = source.pins;
  return out;
}

Status MemoryHotStore::touch_read(const std::string &key, int64_t now_ms, int64_t tick) {
  std::unique_lock lock(mu_);
  auto it = map_.find(key);
  if (it == map_.end())
    return Status::Error("not found", ErrorCode::notFound);
  auto record=it->second;
  record.object.last_read_ms = now_ms;
  record.activity_version += 1;
  record.activity_tick = tick;
  put_record_locked(key,std::move(record));
  return Status::OK();
}

namespace {
size_t trim_add(size_t a, size_t b) {
  if (b > SIZE_MAX - a)
    throw QueryFailure(Status::Error("trim workspace size overflow", ErrorCode::limitExceeded));
  return a + b;
}
size_t trim_record_charge(const GraphObject &object, size_t path_charge) {
  // Encoded size bounds all dynamic identities/properties. Each property has
  // >=5 encoded bytes; 64x covers its map node/string slack, the copied record,
  // codec vector growth, cold manifest, key/cursor and shared control blocks.
  // Update paths share ancestor values; no unrelated payload is copied.
  const auto bytes = encoded_object_bytes(object);
  if (bytes > (SIZE_MAX - 4096) / 64)
    throw QueryFailure(Status::Error("trim record size overflow", ErrorCode::limitExceeded));
  return trim_add(4096 + bytes * 64, path_charge);
}
size_t record_metadata(const std::string &key, const StoredRecord &record) {
  const auto &o = record.object;
  size_t total = PersistentMap<StoredRecord>::entry_overhead_bytes();
  for (auto bytes : {key.capacity(), o.id.capacity(), o.label_or_type.capacity(),
                     o.from.capacity(), o.to.capacity(), record.disk_key.capacity()})
    total = trim_add(total, bytes);
  return total;
}
}
TrimBatch MemoryHotStore::trim_candidates(int64_t cutoff, size_t limit,
    const std::string &after, size_t bytes_to_evict, QueryContext &context) {
  context.enforce();
  limit = std::min(limit, size_t(64));
  TrimBatch batch{context.hold_work(limit * sizeof(TrimCandidate)), {}};
  if (!limit || !bytes_to_evict) return batch;
  batch.candidates.reserve(limit);
  std::shared_lock lock(mu_);
  size_t selected_bytes = 0;
  for (auto it = map_.upper_bound(after); it != map_.end(); ++it) {
    context.enforce();
    const auto &[key, record] = *it;
    if (record.tier != StorageTier::Hot || record.activity_tick > cutoff ||
        (record.pins && record.pins->load())) continue;
    const auto charge = trim_record_charge(record.object, map_.update_path_charge());
    if (charge > context.working_available()) {
      if (!batch.candidates.empty()) break;
      throw QueryFailure(Status::Error("trim workspace cannot hold one candidate", ErrorCode::limitExceeded));
    }
    auto reservation = context.hold_work(charge);
    batch.candidates.push_back({std::move(reservation), key, record});
    selected_bytes = trim_add(selected_bytes, record.payload_bytes);
    if (batch.candidates.size() == limit || selected_bytes >= bytes_to_evict) break;
  }
  return batch;
}
bool MemoryHotStore::publish_trim(const TrimCandidate &candidate,
    const PayloadDescriptor &descriptor, size_t &metadata, size_t metadata_limit,
    QueryContext &context) {
  context.enforce();
  std::unique_lock lock(mu_);
  auto it = map_.find(candidate.key);
  if (it == map_.end()) return false;
  const auto &old = it->second;
  if (old.tier != StorageTier::Hot || (old.pins && old.pins->load()) ||
      old.object.version != candidate.record.object.version ||
      old.activity_version != candidate.record.activity_version) return false;
  // The candidate reservation covers this manifest and the replacement path.
  // Build without copying properties, and inspect actual capacities before swap.
  StoredRecord cold;
  const auto &o = old.object;
  cold.object.kind=o.kind; cold.object.id=o.id; cold.object.label_or_type=o.label_or_type;
  cold.object.from=o.from; cold.object.to=o.to; cold.object.version=o.version;
  cold.object.last_read_ms=o.last_read_ms; cold.object.last_modified_ms=o.last_modified_ms;
  cold.tier=StorageTier::Cold; cold.disk_key=descriptor.filename;
  cold.payload_bytes=descriptor.bytes; cold.payload_crc=descriptor.crc;
  cold.activity_version=old.activity_version; cold.activity_tick=old.activity_tick; cold.pins=old.pins;
  auto replacement = map_;
  replacement.set(candidate.key, std::move(cold));
  auto prepared = replacement.find(candidate.key);
  const auto before = record_metadata(it->first, old);
  const auto after = record_metadata(prepared->first, prepared->second);
  if (before > metadata)
    throw QueryFailure(Status::Error("trim metadata accounting mismatch", ErrorCode::corruptData));
  const auto projected = trim_add(metadata - before, after);
  if (projected > metadata_limit)
    throw QueryFailure(Status::Error("metadata/index budget exceeded during trim", ErrorCode::limitExceeded));
  context.enforce();
  map_.swap(replacement);
  metadata = projected;
  return true;
}
size_t MemoryHotStore::pinned_hot_bytes(QueryContext &context) const {
  std::shared_lock lock(mu_);
  size_t bytes = 0;
  for (const auto &[key, record] : map_) {
    context.enforce();
    if (record.tier == StorageTier::Hot && record.pins && record.pins->load())
      bytes = trim_add(bytes, record.payload_bytes);
  }
  return bytes;
}
std::vector<StoredRecord> MemoryHotStore::cold_candidates(int64_t cutoff_ms, size_t limit) {
  std::shared_lock lock(mu_);
  std::vector<StoredRecord> out;
  out.reserve(limit);
  for (const auto &[_, rec] : map_) {
    if (rec.tier == StorageTier::Hot && rec.activity_tick <= cutoff_ms &&
        (!rec.pins || !rec.pins->load())) {
      out.push_back(rec);
      if (limit > 0 && out.size() >= limit)
        break;
    }
  }
  return out;
}

bool MemoryHotStore::move_to_cold_if_version(const std::string &key, uint64_t version,
                                             const std::string &disk_key, uint64_t bytes,
                                             uint32_t crc, uint64_t activity) {
  auto prepared_name = disk_key;
  std::unique_lock lock(mu_);
  auto it = map_.find(key);
  if (it == map_.end())
    return false;
  if (it->second.tier != StorageTier::Hot || (it->second.pins && it->second.pins->load()) ||
      it->second.object.version != version ||
      (activity != UINT64_MAX && activity != it->second.activity_version))
    return false;
  auto record=it->second;
  record.tier = StorageTier::Cold;
  record.disk_key = std::move(prepared_name);
  record.payload_bytes = bytes;
  record.payload_crc = crc;
  record.object.properties.clear(); // keep only a compact manifest in memory
  put_record_locked(key,std::move(record));
  return true;
}

Status MemoryHotStore::promote_if_cold(const std::string &key, GraphObject object, int64_t now_ms,
                                       int64_t tick) {
  std::unique_lock lock(mu_);
  auto it = map_.find(key);
  if (it == map_.end())
    return Status::Error("cold manifest missing");
  if (it->second.tier != StorageTier::Cold)
    return Status::OK();
  if (object_key(object.kind, object.id) != key || object.version != it->second.object.version ||
      object.label_or_type != it->second.object.label_or_type ||
      object.from != it->second.object.from || object.to != it->second.object.to)
    return Status::Error("payload identity/version mismatch", ErrorCode::corruptData);
  object.last_read_ms = now_ms;
  auto bytes = encoded_object_bytes(object);
  put_record_locked(key,StoredRecord{std::move(object), StorageTier::Hot, {}, bytes, 0, 0, tick, {}});
  return Status::OK();
}

Status MemoryHotStore::remove(const std::string &key) {
  std::unique_lock lock(mu_);
  auto it = map_.find(key);
  if (it == map_.end())
    return Status::Error("not found", ErrorCode::notFound);
  remove_from_indexes_by_key_locked(key);
  map_.erase(key,workspace_);
  return Status::OK();
}

Status MemoryHotStore::delete_node(const std::string &id,
                                   std::vector<std::string> &removed_disk_keys) {
  std::unique_lock lock(mu_);
  auto key = object_key(ObjectKind::Node, id);
  auto it = map_.find(key);
  if (it == map_.end())
    return Status::Error("node not found: " + id, ErrorCode::notFound);

  if (it->second.tier == StorageTier::Cold && !it->second.disk_key.empty()) {
    removed_disk_keys.push_back(it->second.disk_key);
  }
  remove_from_indexes_by_key_locked(key);
  map_.erase(key,workspace_);

  // Cascade delete incident edges
  std::vector<std::string> incident_edge_keys;
  for (const auto &[edge_key, rec] : map_) {
    if (rec.object.kind == ObjectKind::Edge && (rec.object.from == id || rec.object.to == id)) {
      incident_edge_keys.push_back(edge_key);
    }
  }

  for (const auto &edge_key : incident_edge_keys) {
    auto edge_it = map_.find(edge_key);
    if (edge_it != map_.end()) {
      if (edge_it->second.tier == StorageTier::Cold && !edge_it->second.disk_key.empty()) {
        removed_disk_keys.push_back(edge_it->second.disk_key);
      }
      remove_from_indexes_by_key_locked(edge_key);
      map_.erase(edge_key,workspace_);
    }
  }

  return Status::OK();
}

Status MemoryHotStore::delete_edge(const std::string &from, const std::string &type,
                                   const std::string &to, std::string &removed_disk_key) {
  std::unique_lock lock(mu_);
  auto key = object_key(ObjectKind::Edge, edge_id(from, type, to));
  auto it = map_.find(key);
  if (it == map_.end())
    return Status::Error("edge not found", ErrorCode::notFound);

  if (it->second.tier == StorageTier::Cold && !it->second.disk_key.empty()) {
    removed_disk_key = it->second.disk_key;
  }
  remove_from_indexes_by_key_locked(key);
  map_.erase(key,workspace_);
  return Status::OK();
}

void MemoryHotStore::put_record_locked(const std::string &key,StoredRecord record) {
  const auto extra=encoded_object_bytes(record.object)+record.object.properties.size()*256+record.disk_key.size();
  map_.set(key,std::move(record),workspace_,extra);
}
size_t MemoryHotStore::query_clone_charge() const {
  return sizeof(MemoryHotStore)+128;
}
size_t MemoryHotStore::query_record_charge(const std::string &key) const {
  std::shared_lock lock(mu_);
  auto it=map_.find(key);
  if(it==map_.end())return 0;
  // Conservative capacity for decoded property nodes, record copies and serialization scratch.
  return 512 + it->second.payload_bytes*24 + map_.update_path_charge();
}
std::optional<std::string> MemoryHotStore::next_query_key(QueryIndex kind,
    const std::string &identity, const std::string &property, const Property &value,
    const std::string &after, QueryContext &context) {
  context.enforce();
  std::shared_lock lock(mu_);
  const auto *index=&node_label_index_;
  std::string bucket;
  switch(kind) {
    case QueryIndex::Label: bucket=identity; break;
    case QueryIndex::Property: index=&node_property_index_; bucket=property_index_key(identity,property,value); break;
    case QueryIndex::Outbound: index=&outbound_edge_index_; bucket=adjacency_index_key(identity,property); break;
    case QueryIndex::Inbound: index=&inbound_edge_index_; bucket=adjacency_index_key(identity,property); break;
  }
  auto found=index->find(bucket); if(found==index->end())return {};
  auto next=found->second.upper_bound(after); if(next==found->second.end())return {};
  context.reserve_work(2*next->size()+64);
  return *next;
}

std::vector<std::string> MemoryHotStore::node_keys_by_label(const std::string &label,
                                                            size_t limit) {
  std::shared_lock lock(mu_);
  auto it = node_label_index_.find(label);
  if (it == node_label_index_.end())
    return {};
  return sorted_limited(it->second, limit);
}

std::vector<std::string>
MemoryHotStore::node_keys_by_label_property(const std::string &label,
                                            const std::string &property_name, const Property &value,
                                            size_t limit) {
  std::shared_lock lock(mu_);
  auto it = node_property_index_.find(property_index_key(label, property_name, value));
  if (it != node_property_index_.end())
    return sorted_limited(it->second, limit);

  // Fallback scan for unindexed or newly introduced fields
  PersistentSet matches;
  auto label_it = node_label_index_.find(label);
  if (label_it == node_label_index_.end())
    return {};
  for (const auto &key : label_it->second) {
    auto rec_it = map_.find(key);
    if (rec_it == map_.end())
      continue;
    const auto &props = rec_it->second.object.properties;
    auto prop_it = props.find(property_name);
    if (prop_it == props.end())
      continue;
    if (property_to_json(prop_it->second) == property_to_json(value))
      matches.insert(key);
  }
  return sorted_limited(matches, limit);
}

std::vector<std::string> MemoryHotStore::edge_keys_from(const std::string &from,
                                                        const std::string &type, size_t limit) {
  std::shared_lock lock(mu_);
  auto it = outbound_edge_index_.find(adjacency_index_key(from, type));
  if (it == outbound_edge_index_.end())
    return {};
  return sorted_limited(it->second, limit);
}

std::vector<std::string> MemoryHotStore::edge_keys_to(const std::string &to,
                                                      const std::string &type, size_t limit) {
  std::shared_lock lock(mu_);
  auto it = inbound_edge_index_.find(adjacency_index_key(to, type));
  if (it == inbound_edge_index_.end())
    return {};
  return sorted_limited(it->second, limit);
}

Status MemoryHotStore::create_node_property_index(const std::string &label,
                                                  const std::string &property_name, DiskStore *disk) {
  if (label.empty() || property_name.empty())
    return Status::Error("label and property name are required for index creation");
  std::unique_lock lock(mu_);
  auto declaration=declared_index_key(label,property_name);
  if(declared_node_property_indexes_.count(declaration))return Status::OK();
  auto properties=node_property_index_;
  auto declarations=declared_node_property_indexes_;
  declarations.insert(declaration,workspace_);
  for (const auto &[key, record] : map_) {
    if(workspace_)workspace_->enforce();
    if(record.object.kind!=ObjectKind::Node || record.object.label_or_type!=label)continue;
    auto scratch=workspace_?workspace_->hold_work(index_scratch_charge(record,record.tier==StorageTier::Cold && disk)):WorkReservation{};
    std::optional<GraphObject> decoded;
    if(record.tier==StorageTier::Cold && disk) {
      auto read=read_manifest(*disk,record);if(!read)return read.status;
      decoded=std::move(read.value);
    }
    const auto &object=decoded?*decoded:record.object;
    auto found=object.properties.find(property_name);
    if(found!=object.properties.end()) {
      auto key_scratch=workspace_?workspace_->hold_work(property_key_charge(label,property_name,found->second)):WorkReservation{};
      add_posting(properties,property_index_key(label,property_name,found->second),key,workspace_);
    }
  }
  node_property_index_.swap(properties);
  declared_node_property_indexes_.swap(declarations);
  return Status::OK();
}

bool MemoryHotStore::has_node_property_index(const std::string &label,
                                             const std::string &property_name) const {
  std::shared_lock lock(mu_);
  return declared_node_property_indexes_.count(declared_index_key(label, property_name)) > 0;
}

Status MemoryHotStore::rebuild_indexes(DiskStore *disk) {
  std::unique_lock lock(mu_);
  MemoryHotStore rebuilt;
  rebuilt.workspace_=workspace_;
  rebuilt.declared_node_property_indexes_=declared_node_property_indexes_;
  for (const auto &[key, record] : map_) {
    auto scratch=workspace_?workspace_->hold_work(index_scratch_charge(record,record.tier==StorageTier::Cold && disk)):WorkReservation{};
    if(record.tier==StorageTier::Cold && disk) {
      auto read=read_manifest(*disk,record);if(!read)return read.status;
      rebuilt.index_object_locked(key,read.value);
    } else rebuilt.index_object_locked(key,record.object);
  }
  node_label_index_.swap(rebuilt.node_label_index_);
  node_property_index_.swap(rebuilt.node_property_index_);
  outbound_edge_index_.swap(rebuilt.outbound_edge_index_);
  inbound_edge_index_.swap(rebuilt.inbound_edge_index_);
  return Status::OK();
}

size_t encoded_object_bytes(const GraphObject &o) {
  size_t bytes = 57 + o.id.size() + o.label_or_type.size() + o.from.size() + o.to.size();
  for (const auto &[key, value] : o.properties) {
    bytes += 5 + key.size();
    if (auto text = std::get_if<std::string>(&value))
      bytes += 4 + text->size();
    else if (std::holds_alternative<int64_t>(value) || std::holds_alternative<double>(value))
      bytes += 8;
    else if (std::holds_alternative<bool>(value))
      bytes += 1;
  }
  return bytes;
}
MemoryUsage MemoryHotStore::memory_usage(QueryContext *context) const {
  std::shared_lock lock(mu_);
  MemoryUsage usage;
  for (const auto &[key, rec] : map_) {
    if (context) context->enforce();
    usage.metadata_bytes += record_metadata(key, rec);
    if (rec.tier == StorageTier::Hot)
      usage.hot_payload_bytes += rec.payload_bytes;
  }
  for (auto index :
       {&node_label_index_, &node_property_index_, &outbound_edge_index_, &inbound_edge_index_})
    for (const auto &[key, posts] : *index) {
      if (context) context->enforce();
      usage.metadata_bytes += PersistentMap<PersistentSet>::entry_overhead_bytes() + key.capacity();
      usage.metadata_bytes += posts.memory_usage();
    }
  usage.metadata_bytes += declared_node_property_indexes_.memory_usage();
  return usage;
}
std::shared_ptr<void> MemoryHotStore::pin(const std::string &key) {
  std::unique_lock lock(mu_);
  auto it = map_.find(key);
  if (it == map_.end() || it->second.tier != StorageTier::Hot)
    return {};
  auto counter = it->second.pins;
  if (!counter) {
    counter=std::make_shared<std::atomic<size_t>>(0);
    auto record=it->second;record.pins=counter;put_record_locked(key,std::move(record));
  }
  struct Lease {
    std::shared_ptr<std::atomic<size_t>> count;
    explicit Lease(std::shared_ptr<std::atomic<size_t>> value) : count(std::move(value)) {
      ++*count;
    }
    ~Lease() { --*count; }
  };
  return std::make_shared<Lease>(std::move(counter));
}
void MemoryHotStore::reset_activity(int64_t tick) {
  std::unique_lock lock(mu_);
  for (const auto &[key, rec] : map_) {
    auto record=rec;record.activity_tick=tick;put_record_locked(key,std::move(record));
  }
}

size_t MemoryHotStore::hot_count() const {
  std::shared_lock lock(mu_);
  size_t count = 0;
  for (const auto &[_, rec] : map_)
    if (rec.tier == StorageTier::Hot)
      ++count;
  return count;
}

size_t MemoryHotStore::cold_count() const {
  std::shared_lock lock(mu_);
  size_t count = 0;
  for (const auto &[_, rec] : map_)
    if (rec.tier == StorageTier::Cold)
      ++count;
  return count;
}

void MemoryHotStore::visit_cold_references(const std::function<void(std::string_view)> &visit,QueryContext &context) const {
  std::shared_lock lock(mu_);
  for(const auto &[key,record]:map_){context.enforce();if(record.tier==StorageTier::Cold)visit(record.disk_key);}
}
std::vector<StoredRecord> MemoryHotStore::dump_records(bool properties) const {
  std::shared_lock lock(mu_);
  std::vector<StoredRecord> out;
  out.reserve(map_.size());
  for (const auto &[key, rec] : map_) {
    (void)key;
    if (properties)
      out.push_back(rec);
    else {
      StoredRecord copy;
      copy.tier = rec.tier;
      copy.disk_key = rec.disk_key;
      copy.payload_bytes = rec.payload_bytes;
      copy.pins = rec.pins;
      out.push_back(std::move(copy));
    }
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> MemoryHotStore::dump_declared_indexes() const {
  std::shared_lock lock(mu_);
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto &raw : declared_node_property_indexes_) {
    auto colon = raw.find(':');
    auto len = std::stoull(raw.substr(0, colon));
    auto start = colon + 1;
    auto next = start + len;
    auto second = raw.find(':', next);
    out.emplace_back(raw.substr(start, len), raw.substr(second + 1));
  }
  return out;
}

void MemoryHotStore::load_records(
    std::vector<StoredRecord> records,
    std::vector<std::pair<std::string, std::string>> declared_indexes) {
  std::unique_lock lock(mu_);
  map_.clear();
  node_label_index_.clear();
  node_property_index_.clear();
  outbound_edge_index_.clear();
  inbound_edge_index_.clear();
  declared_node_property_indexes_.clear();

  for (const auto &[label, prop] : declared_indexes) {
    declared_node_property_indexes_.insert(declared_index_key(label, prop));
  }

  for (auto &rec : records) {
    auto key = object_key(rec.object.kind, rec.object.id);
    index_object_locked(key, rec.object);
    if (rec.tier == StorageTier::Hot)
      rec.payload_bytes = encoded_object_bytes(rec.object);
    rec.activity_tick = now_millis();
    put_record_locked(key,std::move(rec));
  }
}

IndexState MemoryHotStore::dump_indexes() const {
  std::shared_lock lock(mu_);
  IndexState out;
  size_t section = 0;
  for (auto source :
       {&node_label_index_, &node_property_index_, &outbound_edge_index_, &inbound_edge_index_}) {
    for (const auto &[key, postings] : *source)
      for (const auto &target : postings)
        out.sections[section][key][target] = map_.at(target).object.version;
    ++section;
  }
  return out;
}
void MemoryHotStore::load_indexes(const IndexState &state) {
  std::array<PersistentMap<PersistentSet>, 4> prepared;
  for (size_t i = 0; i < 4; ++i)
    for (const auto &[key, posts] : state.sections[i])
      for (const auto &[object, version] : posts) {
        (void)version;
        add_posting(prepared[i],key,object,workspace_);
      }
  std::unique_lock lock(mu_);
  node_label_index_.swap(prepared[0]);
  node_property_index_.swap(prepared[1]);
  outbound_edge_index_.swap(prepared[2]);
  inbound_edge_index_.swap(prepared[3]);
}
std::shared_ptr<HotStore> MemoryHotStore::clone(QueryContext *context) const {
  auto copy = std::make_shared<MemoryHotStore>();
  std::shared_lock lock(mu_);
  copy->workspace_=context;
  copy->map_ = map_;
  copy->node_label_index_ = node_label_index_;
  copy->node_property_index_ = node_property_index_;
  copy->outbound_edge_index_ = outbound_edge_index_;
  copy->inbound_edge_index_ = inbound_edge_index_;
  copy->declared_node_property_indexes_ = declared_node_property_indexes_;
  return copy;
}
void MemoryHotStore::swap_state(HotStore &other) noexcept {
  auto &state = static_cast<MemoryHotStore &>(other);
  std::scoped_lock lock(mu_, state.mu_);
  map_.swap(state.map_);
  node_label_index_.swap(state.node_label_index_);
  node_property_index_.swap(state.node_property_index_);
  outbound_edge_index_.swap(state.outbound_edge_index_);
  inbound_edge_index_.swap(state.inbound_edge_index_);
  declared_node_property_indexes_.swap(state.declared_node_property_indexes_);
}
Result<PayloadDescriptor> describe_payload(DiskStore &disk, const std::string &name) {
  auto read = disk.read(name);
  if (!read)
    return {read.status, {}};
  auto bytes = serialize_object_binary(read.value);
  return {Status::OK(), {name, bytes.size(), crc32_compute(bytes.data(), bytes.size() - 4)}};
}
Result<GraphObject> read_manifest(DiskStore &disk, const StoredRecord &manifest) {
  auto read = disk.read(manifest.disk_key);
  if (!read) {
    if (read.status.code == ErrorCode::notFound)
      read.status.code = ErrorCode::corruptData;
    return read;
  }
  const auto &object = read.value;
  auto bytes = serialize_object_binary(object);
  if (object.kind != manifest.object.kind || object.id != manifest.object.id ||
      object.label_or_type != manifest.object.label_or_type ||
      object.from != manifest.object.from || object.to != manifest.object.to ||
      object.version != manifest.object.version || bytes.size() != manifest.payload_bytes ||
      crc32_compute(bytes.data(), bytes.size() - 4) != manifest.payload_crc)
    return {Status::Error("cold payload reference mismatch", ErrorCode::corruptData,
                          {{}, 0, manifest.object.id}),
            {}};
  return read;
}

Result<std::vector<std::string>> DiskStore::write_batch(const std::vector<GraphObject> &objects,
                                                       QueryContext &context) {
  std::vector<std::string> names;
  names.reserve(objects.size());
  for (const auto &object : objects) {
    auto status = context.check();
    if (!status.ok)
      return {status, {}};
    auto written = write(object_key(object.kind, object.id), object);
    if (!written)
      return {written.status, {}};
    names.push_back(std::move(written.value));
  }
  return {Status::OK(), std::move(names)};
}

FileDiskStore::FileDiskStore(std::filesystem::path root, std::shared_ptr<FileIO> io)
    : root_(std::move(root)), io_(std::move(io)) {
  std::filesystem::create_directories(root_);
}

Result<std::string> FileDiskStore::write(const std::string &key, const GraphObject &object) {
  std::lock_guard lock(write_mu_);
  (void)key;
  auto disk_key = "payload-" + new_transaction_id() + ".rec";
  auto path = root_ / disk_key;
  auto payload = serialize_object_binary(object);
  auto status = io_->write_all(path, payload, WriteMode::exclusive);
  if (!status.ok)
    return {status, {}};
  io_->fault_point("payload.after_write");
  status = io_->sync_file(path);
  if (!status.ok)
    return {status, {}};
  io_->fault_point("payload.after_sync");
  status = io_->sync_directory(root_);
  if (!status.ok)
    return {status, {}};
  return {Status::OK(), disk_key};
}

namespace {
template<class ObjectAt>
Result<std::vector<PayloadDescriptor>> write_payload_group(
    const std::filesystem::path &root, FileIO &io, size_t count,
    ObjectAt object_at, QueryContext &context) {
  std::vector<PayloadDescriptor> descriptors;
  std::vector<std::filesystem::path> paths;
  descriptors.reserve(count); paths.reserve(count);
  for (size_t i=0; i<count; ++i) {
    context.enforce();
    auto name = "payload-" + new_transaction_id() + ".rec";
    auto path = root / name;
    auto payload = serialize_object_binary(object_at(i));
    auto status = io.write_all(path, payload, WriteMode::exclusive);
    if (!status.ok) return {status, {}};
    descriptors.push_back({std::move(name), payload.size(),
                          crc32_compute(payload.data(), payload.size()-4)});
    paths.push_back(std::move(path));
    io.fault_point("payload.after_write");
  }
  if (paths.empty()) return {Status::OK(), {}};
  context.enforce();
  auto status = io.sync_files(paths);
  if (!status.ok) return {status, {}};
  io.fault_point("payload.after_sync");
  status = io.sync_directory(root);
  if (!status.ok) return {status, {}};
  io.fault_point("payload.group.after_directory_sync");
  return {Status::OK(), std::move(descriptors)};
}
}
Result<std::vector<PayloadDescriptor>> DiskStore::write_trim_batch(
    const std::vector<TrimCandidate> &, QueryContext &) {
  return {Status::Error("disk backend does not support bounded trim writes", ErrorCode::limitExceeded), {}};
}
Result<std::vector<PayloadDescriptor>> FileDiskStore::write_trim_batch(
    const std::vector<TrimCandidate> &candidates, QueryContext &context) {
  if (candidates.size()>64)
    return {Status::Error("trim group exceeds 64 records", ErrorCode::limitExceeded), {}};
  // Candidate leases already cover codec buffers. Reserve path/name vectors here
  // before selection's group is written; native and caller ceilings are tightened.
  const auto per_file = trim_add(1024, trim_add(root_.native().size(), root_.native().size()));
  if (candidates.size() && per_file > (SIZE_MAX-4096)/candidates.size())
    return {Status::Error("trim path size overflow", ErrorCode::limitExceeded), {}};
  auto scratch = context.hold_work(4096 + candidates.size()*per_file);
  std::lock_guard lock(write_mu_);
  return write_payload_group(root_, *io_, candidates.size(),
      [&](size_t i) -> const GraphObject & { return candidates[i].record.object; }, context);
}
Result<std::vector<std::string>> FileDiskStore::write_batch(
    const std::vector<GraphObject> &objects, QueryContext &context) {
  std::lock_guard lock(write_mu_);
  auto result = write_payload_group(root_, *io_, objects.size(),
      [&](size_t i) -> const GraphObject & { return objects[i]; }, context);
  if (!result) return {result.status, {}};
  std::vector<std::string> names;
  names.reserve(result.value.size());
  for (auto &descriptor : result.value) names.push_back(std::move(descriptor.filename));
  return {Status::OK(), std::move(names)};
}

Result<GraphObject> FileDiskStore::read(const std::string &disk_key) {
  if (disk_key.empty() || disk_key.size() > 255 || disk_key.find('\0') != std::string::npos ||
      std::filesystem::path(disk_key).filename() != disk_key || disk_key == "." || disk_key == "..")
    return {Status::Error("invalid payload filename", ErrorCode::corruptData), {}};
  auto bytes = io_->read(root_ / disk_key, kMaxObjectBytes);
  if (!bytes) {
    if (bytes.status.code == ErrorCode::notFound)
      bytes.status.code = ErrorCode::corruptData;
    return {bytes.status, {}};
  }
  return deserialize_object_binary(bytes.value.data(), bytes.value.size());
}
Status FileDiskStore::remove(const std::string &disk_key) {
  std::lock_guard lock(write_mu_);
  if (disk_key.empty() || disk_key.size() > 255 || disk_key.find('\0') != std::string::npos ||
      std::filesystem::path(disk_key).filename() != disk_key || disk_key == "." || disk_key == "..")
    return Status::Error("invalid payload filename", ErrorCode::corruptData);
  auto status = io_->remove(root_ / disk_key);
  if (!status.ok)
    return status;
  return io_->sync_directory(root_);
}

std::string key_to_filename(const std::string &key) {
  std::ostringstream os;
  for (unsigned char c : key) {
    os << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
  }
  os << ".rec";
  return os.str();
}

static std::string encode_props(const PropertyMap &props) {
  std::ostringstream os;
  bool first = true;
  for (const auto &[k, v] : props) {
    if (!first)
      os << ';';
    first = false;
    os << k << '=' << property_to_string(v);
  }
  return os.str();
}

static PropertyMap decode_props(const std::string &s) {
  PropertyMap props;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ';')) {
    if (item.empty())
      continue;
    auto eq = item.find('=');
    if (eq == std::string::npos)
      continue;
    props[item.substr(0, eq)] = parse_scalar(item.substr(eq + 1));
  }
  return props;
}

std::string serialize_object(const GraphObject &object) {
  std::ostringstream os;
  os << static_cast<int>(object.kind) << '\n'
     << object.id << '\n'
     << object.label_or_type << '\n'
     << object.from << '\n'
     << object.to << '\n'
     << object.last_read_ms << '\n'
     << object.last_modified_ms << '\n'
     << object.version << '\n'
     << encode_props(object.properties) << '\n';
  return os.str();
}

Result<GraphObject> deserialize_object(const std::string &payload) {
  std::stringstream ss(payload);
  std::string line;
  GraphObject obj;
  try {
    std::getline(ss, line);
    obj.kind = static_cast<ObjectKind>(std::stoi(line));
    std::getline(ss, obj.id);
    std::getline(ss, obj.label_or_type);
    std::getline(ss, obj.from);
    std::getline(ss, obj.to);
    std::getline(ss, line);
    obj.last_read_ms = std::stoll(line);
    std::getline(ss, line);
    obj.last_modified_ms = std::stoll(line);
    std::getline(ss, line);
    obj.version = static_cast<uint64_t>(std::stoull(line));
    std::getline(ss, line);
    obj.properties = decode_props(line);
  } catch (const std::exception &e) {
    return {Status::Error(std::string("invalid cold record: ") + e.what()), {}};
  }
  return {Status::OK(), obj};
}

} // namespace graphdb

namespace graphdb {
std::vector<std::string> MemoryHotStore::inspection_keys(unsigned kind, const std::string &after,
    size_t limit, QueryContext &context) const {
  // Bounded selection over metadata; no whole-database copy and no payload reads.
  std::shared_lock lock(mu_);
  context.reserve_work(limit * 96);
  std::set<std::string_view> selected;
  auto consider = [&](const std::string &key) {
    if (key <= after) return;
    if (selected.size() == limit && key >= *selected.rbegin()) return;
    selected.insert(key);
    if (selected.size() > limit) selected.erase(std::prev(selected.end()));
  };
  if (kind == 2) {
    for (const auto &key : declared_node_property_indexes_) { context.expand_edge(); consider(key); }
  } else {
    for (const auto &[key, record] : map_) {
      context.expand_edge();
      if ((kind == 0) == (record.object.kind == ObjectKind::Node)) consider(key);
    }
  }
  std::vector<std::string> result;
  for (auto key : selected) { context.reserve_work(key.size()*2+64); result.emplace_back(key); }
  return result;
}
} // namespace graphdb
