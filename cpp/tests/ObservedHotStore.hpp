#pragma once
#include "graphdb/Storage.hpp"

namespace nova_test {
// Real storage with accounting-call observation; no graph/index behavior is mocked.
class ObservedHotStore : public graphdb::HotStore {
public:
  graphdb::MemoryHotStore store;
  mutable size_t accounting_scans{0};
  graphdb::MemoryUsage memory_usage(graphdb::QueryContext *context = nullptr) const override {
    ++accounting_scans;
    return store.memory_usage(context);
  }
  std::optional<std::string> next_query_key(QueryIndex i, const std::string &a, const std::string &p, const graphdb::Property &v, const std::string &after, graphdb::QueryContext &c) override { return store.next_query_key(i,a,p,v,after,c); }
  size_t query_clone_charge() const override { return store.query_clone_charge(); }
  std::vector<std::string> inspection_keys(unsigned kind, const std::string &after, size_t limit, graphdb::QueryContext &context) const override { return store.inspection_keys(kind,after,limit,context); }
  size_t query_record_charge(const std::string &key) const override { return store.query_record_charge(key); }
  graphdb::Status put_hot(const std::string &k, graphdb::GraphObject o) override { return store.put_hot(k, std::move(o)); }
  std::optional<graphdb::StoredRecord> get(const std::string &k, bool p = true) override { return store.get(k, p); }
  graphdb::Status touch_read(const std::string &k, int64_t n, int64_t t) override { return store.touch_read(k, n, t); }
  graphdb::TrimBatch trim_candidates(int64_t cutoff, size_t limit, const std::string &after, size_t bytes, graphdb::QueryContext &context) override { return store.trim_candidates(cutoff,limit,after,bytes,context); }
  bool publish_trim(const graphdb::TrimCandidate &candidate, const graphdb::PayloadDescriptor &descriptor, size_t &metadata, size_t limit, graphdb::QueryContext &context) override { return store.publish_trim(candidate,descriptor,metadata,limit,context); }
  size_t pinned_hot_bytes(graphdb::QueryContext &context) const override { return store.pinned_hot_bytes(context); }
  std::vector<graphdb::StoredRecord> cold_candidates(int64_t c, size_t n) override { return store.cold_candidates(c, n); }
  bool move_to_cold_if_version(const std::string &k, uint64_t v, const std::string &d, uint64_t b, uint32_t c, uint64_t a) override { return store.move_to_cold_if_version(k, v, d, b, c, a); }
  graphdb::Status promote_if_cold(const std::string &k, graphdb::GraphObject o, int64_t n, int64_t t) override { return store.promote_if_cold(k, std::move(o), n, t); }
  graphdb::Status remove(const std::string &k) override { return store.remove(k); }
  graphdb::Status delete_node(const std::string &k, std::vector<std::string> &d) override { return store.delete_node(k, d); }
  graphdb::Status delete_edge(const std::string &f, const std::string &t, const std::string &to, std::string &d) override { return store.delete_edge(f, t, to, d); }
  std::vector<std::string> node_keys_by_label(const std::string &l, size_t n) override { return store.node_keys_by_label(l, n); }
  std::vector<std::string> node_keys_by_label_property(const std::string &l, const std::string &k, const graphdb::Property &v, size_t n) override { return store.node_keys_by_label_property(l, k, v, n); }
  std::vector<std::string> edge_keys_from(const std::string &f, const std::string &t, size_t n) override { return store.edge_keys_from(f, t, n); }
  std::vector<std::string> edge_keys_to(const std::string &to, const std::string &t, size_t n) override { return store.edge_keys_to(to, t, n); }
  graphdb::Status create_node_property_index(const std::string &l, const std::string &p, graphdb::DiskStore *disk=nullptr) override { return store.create_node_property_index(l, p, disk); }
  bool has_node_property_index(const std::string &l, const std::string &p) const override { return store.has_node_property_index(l, p); }
  graphdb::Status rebuild_indexes(graphdb::DiskStore *disk=nullptr) override { return store.rebuild_indexes(disk); }
  graphdb::IndexState dump_indexes() const override { return store.dump_indexes(); }
  void load_indexes(const graphdb::IndexState &i) override { store.load_indexes(i); }
  std::shared_ptr<graphdb::HotStore> clone(graphdb::QueryContext *context=nullptr) const override { return store.clone(context); }
  void swap_state(graphdb::HotStore &other) noexcept override { store.swap_state(other); }
  std::shared_ptr<void> pin(const std::string &k) override { return store.pin(k); }
  void reset_activity(int64_t t) override { store.reset_activity(t); }
  size_t hot_count() const override { return store.hot_count(); }
  size_t cold_count() const override { return store.cold_count(); }
  void visit_cold_references(const std::function<void(std::string_view)> &visit,graphdb::QueryContext &context) const override {store.visit_cold_references(visit,context);}
  std::vector<graphdb::StoredRecord> dump_records(bool p = true) const override { return store.dump_records(p); }
  std::vector<std::pair<std::string, std::string>> dump_declared_indexes() const override { return store.dump_declared_indexes(); }
  void load_records(std::vector<graphdb::StoredRecord> r, std::vector<std::pair<std::string, std::string>> i) override { store.load_records(std::move(r), std::move(i)); }
};
} // namespace nova_test
