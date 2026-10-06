#include "graphdb/GraphTypes.hpp"
#include "graphdb/ValueCodec.hpp"
#include <cmath>

#include <algorithm>
#include <cctype>

namespace graphdb {

std::string trim(std::string s) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

Property parse_scalar(std::string value) {
  value = trim(std::move(value));
  if (value.empty())
    return std::monostate{};
  if (value == "null")
    return std::monostate{};
  if (value == "true")
    return true;
  if (value == "false")
    return false;
  if ((value.front() == '"' && value.back() == '"') ||
      (value.front() == '\'' && value.back() == '\'')) {
    return value.substr(1, value.size() - 2);
  }
  if (std::isdigit(static_cast<unsigned char>(value.front())) || value.front() == '-' ||
      value.front() == '+') {
    auto parsed = parse_json_properties("{\"value\":" + value + "}");
    return parsed.at("value");
  }

  return value;
}

std::string properties_to_json(const PropertyMap &properties) {
  std::string out = "{";
  bool first = true;
  for (const auto &[k, v] : properties) {
    if (!first)
      out += ",";
    first = false;
    out += "\"" + json_escape(k) + "\":" + property_to_json(v);
  }
  out += "}";
  return out;
}

std::string object_to_json(const GraphObject &obj, const std::vector<std::string> &projection) {
  if (projection.empty()) {
    std::string out = "{";
    out += "\"kind\":\"" + std::string(kind_to_string(obj.kind)) + "\"";
    out += ",\"id\":\"" + json_escape(obj.id) + "\"";
    if (obj.kind == ObjectKind::Node) {
      out += ",\"label\":\"" + json_escape(obj.label_or_type) + "\"";
    } else {
      out += ",\"type\":\"" + json_escape(obj.label_or_type) + "\"";
      out += ",\"from\":\"" + json_escape(obj.from) + "\"";
      out += ",\"to\":\"" + json_escape(obj.to) + "\"";
    }
    out += ",\"properties\":" + properties_to_json(obj.properties);
    out += ",\"last_read_ms\":" + std::to_string(obj.last_read_ms);
    out += ",\"last_modified_ms\":" + std::to_string(obj.last_modified_ms);
    out += ",\"version\":" + std::to_string(obj.version);
    out += "}";
    return out;
  }

  std::string out = "{";
  bool first = true;
  for (const auto &field : projection) {
    if (!first)
      out += ",";
    first = false;
    out += "\"" + json_escape(field) + "\":";
    if (field == "id") {
      out += "\"" + json_escape(obj.id) + "\"";
    } else if (field == "label" || field == "type") {
      out += "\"" + json_escape(obj.label_or_type) + "\"";
    } else if (field == "from") {
      out += "\"" + json_escape(obj.from) + "\"";
    } else if (field == "to") {
      out += "\"" + json_escape(obj.to) + "\"";
    } else if (field == "kind") {
      out += "\"" + std::string(kind_to_string(obj.kind)) + "\"";
    } else if (field == "version") {
      out += std::to_string(obj.version);
    } else {
      auto it = obj.properties.find(field);
      if (it != obj.properties.end()) {
        out += property_to_json(it->second);
      } else {
        out += "null";
      }
    }
  }
  out += "}";
  return out;
}

bool compare_properties(const Property &lhs, const std::string &op, const Property &rhs) {
  if (std::holds_alternative<std::monostate>(lhs) || std::holds_alternative<std::monostate>(rhs)) {
    if (op == "=" || op == "==") {
      return std::holds_alternative<std::monostate>(lhs) &&
             std::holds_alternative<std::monostate>(rhs);
    }
    if (op == "!=") {
      return !(std::holds_alternative<std::monostate>(lhs) &&
               std::holds_alternative<std::monostate>(rhs));
    }
    return false;
  }

  auto is_num = [](const Property &p) {
    return std::holds_alternative<int64_t>(p) || std::holds_alternative<double>(p);
  };
  if (is_num(lhs) && is_num(rhs)) {
    int ordering = 0;
    auto mixed = [](int64_t integer, double real) {
      if (!std::isfinite(real))
        throw std::invalid_argument("nonfinite comparison");
      if (real >= 9223372036854775808.0)
        return -1;
      if (real < -9223372036854775808.0)
        return 1;
      auto truncated = static_cast<int64_t>(real);
      if (integer < truncated)
        return -1;
      if (integer > truncated)
        return 1;
      auto rounded = static_cast<double>(truncated);
      return real > rounded ? -1 : (real < rounded ? 1 : 0);
    };
    if (auto l = std::get_if<int64_t>(&lhs)) {
      if (auto r = std::get_if<int64_t>(&rhs))
        ordering = *l < *r ? -1 : (*l > *r ? 1 : 0);
      else
        ordering = mixed(*l, std::get<double>(rhs));
    } else if (auto r = std::get_if<int64_t>(&rhs))
      ordering = -mixed(*r, std::get<double>(lhs));
    else {
      auto left = std::get<double>(lhs), right = std::get<double>(rhs);
      if (!std::isfinite(left) || !std::isfinite(right))
        throw std::invalid_argument("nonfinite comparison");
      ordering = left < right ? -1 : (left > right ? 1 : 0);
    }
    if (op == "=" || op == "==")
      return ordering == 0;
    if (op == "!=")
      return ordering != 0;
    if (op == "<")
      return ordering < 0;
    if (op == "<=")
      return ordering <= 0;
    if (op == ">")
      return ordering > 0;
    if (op == ">=")
      return ordering >= 0;
    return false;
  }

  if (std::holds_alternative<std::string>(lhs) && std::holds_alternative<std::string>(rhs)) {
    const auto &l = std::get<std::string>(lhs);
    const auto &r = std::get<std::string>(rhs);
    if (op == "=" || op == "==")
      return l == r;
    if (op == "!=")
      return l != r;
    if (op == "<")
      return l < r;
    if (op == "<=")
      return l <= r;
    if (op == ">")
      return l > r;
    if (op == ">=")
      return l >= r;
    return false;
  }

  if (std::holds_alternative<bool>(lhs) && std::holds_alternative<bool>(rhs)) {
    bool l = std::get<bool>(lhs);
    bool r = std::get<bool>(rhs);
    if (op == "=" || op == "==")
      return l == r;
    if (op == "!=")
      return l != r;
    return false;
  }

  if (op == "!=")
    return true;
  return false;
}

} // namespace graphdb
