#pragma once
#include "graphdb/BinaryEncoding.hpp"
#include <array>
#include <variant>
namespace graphdb {
struct UpsertNode {
  std::string label, id;
  PropertyMap properties;
  std::optional<int64_t> modified_ms{};
};
struct UpsertEdge {
  std::string type, from, to;
  PropertyMap properties;
  std::optional<int64_t> modified_ms{};
};
struct DeleteNode {
  std::string id;
};
struct DeleteEdge {
  std::string from, type, to;
};
struct CreateIndex {
  std::string label, property;
};
using Mutation = std::variant<UpsertNode, UpsertEdge, DeleteNode, DeleteEdge, CreateIndex>;
struct TransactionBatch {
  std::string transaction_id;
  std::vector<Mutation> mutations;
};
struct CommitReceipt {
  std::string transaction_id;
  uint64_t committed_lsn{0};
};
struct RecoveredTransaction {
  TransactionBatch batch;
  uint64_t committed_lsn{0};
};
constexpr size_t kMaxBatchStatements = 1000, kMaxBatchBytes = 16 * 1024 * 1024;
using UUID = std::array<uint8_t, 16>;
std::string new_transaction_id();
UUID parse_uuid(std::string_view value);
std::string uuid_string(const UUID &value);
TransactionBatch stamp_modifications(const TransactionBatch &, int64_t);
std::vector<uint8_t> encode_mutation(const Mutation &mutation);
Result<Mutation> decode_mutation(const uint8_t *bytes, size_t size);
} // namespace graphdb
