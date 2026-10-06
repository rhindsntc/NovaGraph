#pragma once
#include "graphdb/Status.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace graphdb {

using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

inline int64_t now_millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline int64_t wall_millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

using Property = std::variant<std::monostate, bool, int64_t, double, std::string>;
using PropertyMap = std::map<std::string, Property>;

enum class ObjectKind : uint8_t { Node = 1, Edge = 2 };
enum class StorageTier : uint8_t { Hot = 1, Cold = 2 };

struct GraphObject {
  ObjectKind kind{ObjectKind::Node};
  std::string id;
  std::string label_or_type;
  std::string from;
  std::string to;
  PropertyMap properties;
  int64_t last_read_ms{0};
  int64_t last_modified_ms{0};
  uint64_t version{0};

  int64_t last_activity_ms() const {
    return last_read_ms > last_modified_ms ? last_read_ms : last_modified_ms;
  }
};

// Node sequences follow traversal order. Inbound edges retain their stored orientation.
struct GraphPath {
  std::vector<GraphObject> nodes;
  std::vector<GraphObject> edges;
};
struct TraversalResult {
  std::vector<GraphObject> nodes;
  std::vector<GraphPath> paths;
};

inline std::string kind_to_string(ObjectKind kind) {
  return kind == ObjectKind::Node ? "node" : "edge";
}

std::string property_to_string(const Property &p);

std::string json_escape(std::string_view in);
std::string property_to_json(const Property &value);

inline std::string object_key(ObjectKind kind, std::string_view id) {
  std::string key = kind == ObjectKind::Node ? "n/" : "e/";
  key += id;
  return key;
}

std::string edge_id(std::string_view from, std::string_view type, std::string_view to);

std::string object_to_json(const GraphObject &obj, const std::vector<std::string> &projection = {});
std::string properties_to_json(const PropertyMap &properties);
Property parse_scalar(std::string value);
std::string trim(std::string s);
bool compare_properties(const Property &lhs, const std::string &op, const Property &rhs);

} // namespace graphdb
