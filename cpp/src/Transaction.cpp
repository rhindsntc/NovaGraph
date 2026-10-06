#include "graphdb/Transaction.hpp"
#include <random>
namespace graphdb {
UUID parse_uuid(std::string_view value) {
  if (value.size() != 32)
    throw std::invalid_argument("transaction UUID must have 32 hex digits");
  UUID out{};
  auto digit = [](char c) -> unsigned {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    throw std::invalid_argument("invalid transaction UUID");
  };
  for (size_t i = 0; i < 16; ++i)
    out[i] = uint8_t(digit(value[i * 2]) * 16 + digit(value[i * 2 + 1]));
  return out;
}
std::string uuid_string(const UUID &value) {
  std::string out;
  out.reserve(32);
  for (auto byte : value) {
    out += '0';
    out.back() = "0123456789abcdef"[byte >> 4];
    out += "0123456789abcdef"[byte & 15];
  }
  return out;
}
std::string new_transaction_id() {
  std::random_device random;
  UUID bytes{};
  for (auto &byte : bytes)
    byte = uint8_t(random());
  return uuid_string(bytes);
}
TransactionBatch stamp_modifications(const TransactionBatch &source, int64_t timestamp) {
  auto batch = source;
  for (auto &mutation : batch.mutations)
    std::visit(
        [&](auto &m) {
          using T = std::decay_t<decltype(m)>;
          if constexpr (std::is_same_v<T, UpsertNode> || std::is_same_v<T, UpsertEdge>)
            if (!m.modified_ms)
              m.modified_ms = timestamp;
        },
        mutation);
  return batch;
}
std::vector<uint8_t> encode_mutation(const Mutation &mutation) {
  std::vector<uint8_t> bytes;
  encode_u8(bytes, uint8_t(mutation.index() + 1));
  std::visit(
      [&](const auto &m) {
        using T = std::decay_t<decltype(m)>;
        auto identity = [&](const std::string &s) {
          validate_identity(s);
          encode_string(bytes, s);
        };
        if constexpr (std::is_same_v<T, UpsertNode>) {
          identity(m.label);
          identity(m.id);
          validate_properties(m.properties);
          encode_properties(bytes, m.properties);
          encode_i64(bytes, m.modified_ms.value_or(wall_millis()));
        }
        if constexpr (std::is_same_v<T, UpsertEdge>) {
          identity(m.type);
          identity(m.from);
          identity(m.to);
          validate_properties(m.properties);
          encode_properties(bytes, m.properties);
          encode_i64(bytes, m.modified_ms.value_or(wall_millis()));
        }
        if constexpr (std::is_same_v<T, DeleteNode>)
          identity(m.id);
        if constexpr (std::is_same_v<T, DeleteEdge>) {
          identity(m.from);
          identity(m.type);
          identity(m.to);
        }
        if constexpr (std::is_same_v<T, CreateIndex>) {
          identity(m.label);
          identity(m.property);
        }
      },
      mutation);
  if (bytes.size() > kMaxBatchBytes)
    throw std::length_error("mutation exceeds batch byte limit");
  return bytes;
}
Result<Mutation> decode_mutation(const uint8_t *bytes, size_t size) {
  ByteReader r(bytes, size);
  uint8_t op;
  Mutation out;
  bool ok = r.read_u8(op);
  if (ok)
    switch (op) {
    case 1: {
      UpsertNode m;
      ok = r.read_string(m.label) && r.read_string(m.id) && r.read_properties(m.properties);
      int64_t modified;
      ok = ok && r.read_i64(modified);
      if (ok)
        m.modified_ms = modified;
      out = std::move(m);
      break;
    }
    case 2: {
      UpsertEdge m;
      ok = r.read_string(m.type) && r.read_string(m.from) && r.read_string(m.to) &&
           r.read_properties(m.properties);
      int64_t modified;
      ok = ok && r.read_i64(modified);
      if (ok)
        m.modified_ms = modified;
      out = std::move(m);
      break;
    }
    case 3: {
      DeleteNode m;
      ok = r.read_string(m.id);
      out = std::move(m);
      break;
    }
    case 4: {
      DeleteEdge m;
      ok = r.read_string(m.from) && r.read_string(m.type) && r.read_string(m.to);
      out = std::move(m);
      break;
    }
    case 5: {
      CreateIndex m;
      ok = r.read_string(m.label) && r.read_string(m.property);
      out = std::move(m);
      break;
    }
    default:
      ok = false;
    }
  if (!ok || r.remaining())
    return {Status::Error("invalid WAL mutation encoding", ErrorCode::corruptData), {}};
  try {
    encode_mutation(out);
  } catch (const std::exception &e) {
    return {Status::Error(e.what(), ErrorCode::corruptData), {}};
  }
  return {Status::OK(), std::move(out)};
}
} // namespace graphdb
