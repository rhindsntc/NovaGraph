#pragma once

#include "graphdb/FileIO.hpp"
#include "graphdb/GraphTypes.hpp"
#include "graphdb/Maintenance.hpp"
#include "graphdb/QueryContext.hpp"
#include "graphdb/PersistentSet.hpp"
#include <atomic>
#include <functional>

#include <array>
#include <filesystem>
#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace graphdb {

struct StoredRecord {
  GraphObject object;
  StorageTier tier{StorageTier::Hot};
  std::string disk_key;
  uint64_t payload_bytes{0};
  uint32_t payload_crc{0};
  uint64_t activity_version{0};
  int64_t activity_tick{0};
  std::shared_ptr<std::atomic<size_t>> pins;
};

struct IndexState {
  using Section = std::map<std::string, std::map<std::string, uint64_t>>;
  std::array<Section, 4> sections;
  bool operator==(const IndexState &) const = default;
};
struct PayloadDescriptor {
  std::string filename;
  uint64_t bytes;
  uint32_t crc;
};
// Reservations precede their allocations so destruction releases memory first.
struct TrimCandidate {
  WorkReservation reservation;
  std::string key;
  StoredRecord record;
};
struct TrimBatch {
  WorkReservation reservation;
  std::vector<TrimCandidate> candidates;
};
// In-process hot tier used by the embedded engine.
class DiskStore;
class HotStore {
public:
  virtual ~HotStore() = default;
  virtual Status put_hot(const std::string &key, GraphObject object) = 0;
  virtual std::optional<StoredRecord> get(const std::string &key, bool properties = true) = 0;
  virtual Status touch_read(const std::string &key, int64_t now_ms,
                            int64_t tick = now_millis()) = 0;
  virtual TrimBatch trim_candidates(int64_t cutoff, size_t limit, const std::string &after,
      size_t bytes_to_evict, QueryContext &) = 0;
  virtual bool publish_trim(const TrimCandidate &, const PayloadDescriptor &,
      size_t &metadata_bytes, size_t metadata_limit, QueryContext &) = 0;
  virtual size_t pinned_hot_bytes(QueryContext &) const = 0;
  virtual std::vector<StoredRecord> cold_candidates(int64_t cutoff_ms, size_t limit) = 0;
  virtual bool move_to_cold_if_version(const std::string &key, uint64_t version,
                                       const std::string &disk_key, uint64_t bytes = 0,
                                       uint32_t crc = 0, uint64_t activity = UINT64_MAX) = 0;
  virtual Status promote_if_cold(const std::string &key, GraphObject object, int64_t now_ms,
                                 int64_t tick = now_millis()) = 0;

  virtual Status remove(const std::string &key) = 0;
  virtual Status delete_node(const std::string &id,
                             std::vector<std::string> &removed_disk_keys) = 0;
  virtual Status delete_edge(const std::string &from, const std::string &type,
                             const std::string &to, std::string &removed_disk_key) = 0;

  // One posting at a time; callers hold the engine gate across a cursor.
  enum class QueryIndex { Label, Property, Outbound, Inbound };
  virtual std::optional<std::string> next_query_key(QueryIndex, const std::string &,
      const std::string &, const Property &, const std::string &, QueryContext &) = 0;
  virtual std::vector<std::string> inspection_keys(unsigned kind, const std::string &after,
      size_t limit, QueryContext &) const = 0;
  virtual size_t query_record_charge(const std::string &) const = 0;
  virtual size_t query_clone_charge() const = 0;
  // Index-backed lookup surfaces.
  virtual std::vector<std::string> node_keys_by_label(const std::string &label, size_t limit) = 0;
  virtual std::vector<std::string> node_keys_by_label_property(const std::string &label,
                                                               const std::string &property_name,
                                                               const Property &value,
                                                               size_t limit) = 0;
  virtual std::vector<std::string> edge_keys_from(const std::string &from, const std::string &type,
                                                  size_t limit) = 0;
  virtual std::vector<std::string> edge_keys_to(const std::string &to, const std::string &type,
                                                size_t limit) = 0;
  virtual Status create_node_property_index(const std::string &label,
                                            const std::string &property_name, DiskStore *disk = nullptr) = 0;
  virtual bool has_node_property_index(const std::string &label,
                                       const std::string &property_name) const = 0;
  virtual Status rebuild_indexes(DiskStore *disk = nullptr) = 0;
  virtual IndexState dump_indexes() const = 0;
  virtual void load_indexes(const IndexState &) = 0;
  virtual std::shared_ptr<HotStore> clone(QueryContext *context = nullptr) const = 0;
  virtual void swap_state(HotStore &) noexcept = 0;
  virtual MemoryUsage memory_usage(QueryContext *context = nullptr) const = 0;
  virtual std::shared_ptr<void> pin(const std::string &) = 0;
  virtual void reset_activity(int64_t) = 0;
  virtual size_t hot_count() const = 0;
  virtual size_t cold_count() const = 0;

  virtual void visit_cold_references(const std::function<void(std::string_view)> &, QueryContext &) const = 0;
  virtual std::vector<StoredRecord> dump_records(bool properties = true) const = 0;
  virtual std::vector<std::pair<std::string, std::string>> dump_declared_indexes() const = 0;
  virtual void load_records(std::vector<StoredRecord> records,
                            std::vector<std::pair<std::string, std::string>> declared_indexes) = 0;
};

class DiskStore {
public:
  virtual ~DiskStore() = default;
  virtual Result<std::string> write(const std::string &key, const GraphObject &object) = 0;
  // Returns only after every payload in this bounded group is durable.
  virtual Result<std::vector<std::string>> write_batch(const std::vector<GraphObject> &,
                                                       QueryContext &);
  // Default rejects before side effects; legacy writers cannot promise a bound.
  virtual Result<std::vector<PayloadDescriptor>> write_trim_batch(
      const std::vector<TrimCandidate> &, QueryContext &);
  virtual Result<GraphObject> read(const std::string &disk_key) = 0;
  virtual Status remove(const std::string &disk_key) = 0;
};

// Indexed, embedded hot tier.
class MemoryHotStore final : public HotStore {
public:
  Status put_hot(const std::string &key, GraphObject object) override;
  std::optional<StoredRecord> get(const std::string &key, bool properties = true) override;
  Status touch_read(const std::string &key, int64_t now_ms, int64_t tick = now_millis()) override;
  TrimBatch trim_candidates(int64_t cutoff, size_t limit, const std::string &after,
      size_t bytes_to_evict, QueryContext &) override;
  bool publish_trim(const TrimCandidate &, const PayloadDescriptor &,
      size_t &metadata_bytes, size_t metadata_limit, QueryContext &) override;
  size_t pinned_hot_bytes(QueryContext &) const override;
  std::vector<StoredRecord> cold_candidates(int64_t cutoff_ms, size_t limit) override;
  bool move_to_cold_if_version(const std::string &key, uint64_t version,
                               const std::string &disk_key, uint64_t bytes = 0, uint32_t crc = 0,
                               uint64_t activity = UINT64_MAX) override;
  Status promote_if_cold(const std::string &key, GraphObject object, int64_t now_ms,
                         int64_t tick = now_millis()) override;

  Status remove(const std::string &key) override;
  Status delete_node(const std::string &id, std::vector<std::string> &removed_disk_keys) override;
  Status delete_edge(const std::string &from, const std::string &type, const std::string &to,
                     std::string &removed_disk_key) override;

  std::optional<std::string> next_query_key(QueryIndex, const std::string &,
      const std::string &, const Property &, const std::string &, QueryContext &) override;
  std::vector<std::string> inspection_keys(unsigned kind, const std::string &after,
      size_t limit, QueryContext &) const override;
  size_t query_record_charge(const std::string &) const override;
  size_t query_clone_charge() const override;
  std::vector<std::string> node_keys_by_label(const std::string &label, size_t limit) override;
  std::vector<std::string> node_keys_by_label_property(const std::string &label,
                                                       const std::string &property_name,
                                                       const Property &value,
                                                       size_t limit) override;
  std::vector<std::string> edge_keys_from(const std::string &from, const std::string &type,
                                          size_t limit) override;
  std::vector<std::string> edge_keys_to(const std::string &to, const std::string &type,
                                        size_t limit) override;
  Status create_node_property_index(const std::string &label,
                                    const std::string &property_name, DiskStore *disk = nullptr) override;
  bool has_node_property_index(const std::string &label,
                               const std::string &property_name) const override;
  Status rebuild_indexes(DiskStore *disk = nullptr) override;
  IndexState dump_indexes() const override;
  void load_indexes(const IndexState &) override;
  std::shared_ptr<HotStore> clone(QueryContext *context = nullptr) const override;
  void swap_state(HotStore &) noexcept override;
  MemoryUsage memory_usage(QueryContext *context = nullptr) const override;
  std::shared_ptr<void> pin(const std::string &) override;
  void reset_activity(int64_t) override;
  size_t hot_count() const override;
  size_t cold_count() const override;

  void visit_cold_references(const std::function<void(std::string_view)> &, QueryContext &) const override;
  std::vector<StoredRecord> dump_records(bool properties = true) const override;
  std::vector<std::pair<std::string, std::string>> dump_declared_indexes() const override;
  void load_records(std::vector<StoredRecord> records,
                    std::vector<std::pair<std::string, std::string>> declared_indexes) override;

private:
  static std::string property_index_key(const std::string &label, const std::string &property_name,
                                        const Property &value);
  static std::string adjacency_index_key(const std::string &node, const std::string &type);
  static std::string declared_index_key(const std::string &label, const std::string &property_name);
  static std::vector<std::string> sorted_limited(const PersistentSet &keys,
                                                 size_t limit);

  void remove_from_indexes_by_key_locked(const std::string &key);
  void index_object_locked(const std::string &key, const GraphObject &object);

  mutable std::shared_mutex mu_;
  PersistentMap<StoredRecord> map_;
  PersistentMap<PersistentSet> node_label_index_;
  PersistentMap<PersistentSet> node_property_index_;
  PersistentMap<PersistentSet> outbound_edge_index_;
  PersistentMap<PersistentSet> inbound_edge_index_;
  PersistentSet declared_node_property_indexes_;
  QueryContext *workspace_{nullptr};
  void put_record_locked(const std::string &, StoredRecord);
};

// Deterministic file-backed cold store with atomic writes.
class FileDiskStore final : public DiskStore {
public:
  explicit FileDiskStore(std::filesystem::path root,
                         std::shared_ptr<FileIO> io = default_file_io());
  Result<std::string> write(const std::string &key, const GraphObject &object) override;
  Result<std::vector<std::string>> write_batch(const std::vector<GraphObject> &,
                                               QueryContext &) override;
  Result<std::vector<PayloadDescriptor>> write_trim_batch(
      const std::vector<TrimCandidate> &, QueryContext &) override;
  Result<GraphObject> read(const std::string &disk_key) override;
  Status remove(const std::string &disk_key) override;

private:
  std::filesystem::path root_;
  std::shared_ptr<FileIO> io_;
  std::mutex write_mu_;
};

size_t encoded_object_bytes(const GraphObject &);
Result<PayloadDescriptor> describe_payload(DiskStore &, const std::string &);
Result<GraphObject> read_manifest(DiskStore &, const StoredRecord &);
std::string serialize_object(const GraphObject &object);
Result<GraphObject> deserialize_object(const std::string &payload);
std::string key_to_filename(const std::string &key);

} // namespace graphdb
