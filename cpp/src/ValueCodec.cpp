#include "graphdb/ValueCodec.hpp"
#include "nlohmann/json.hpp"
#include <cmath>
#include <limits>
#include <set>
namespace graphdb {
using Json = nlohmann::json;
namespace {
struct FlatSax : nlohmann::json_sax<Json> {
  PropertyMap values;
  std::string current, error;
  bool started = false, complete = false, pending = false;
  bool reject(const char *message) {
    error = message;
    return false;
  }
  bool add(Property value) {
    if (!started || complete || !pending)
      return reject("expected flat JSON object");
    values.emplace(std::move(current), std::move(value));
    pending = false;
    return true;
  }
  bool null() override { return add(std::monostate{}); }
  bool boolean(bool value) override { return add(value); }
  bool number_integer(number_integer_t value) override { return add(int64_t(value)); }
  bool number_unsigned(number_unsigned_t value) override {
    if (value > uint64_t(INT64_MAX))
      return reject("integer outside Int64");
    return add(int64_t(value));
  }
  bool number_float(number_float_t value, const string_t &token) override {
    if (token.find_first_not_of("-0123456789") == std::string::npos)
      return reject("integer outside Int64");
    // The JSON lexer already validates syntax and converts with its locale-aware
    // decimal separator. Reject overflow and nonzero literals rounded to zero
    // without floating from_chars (unavailable on our older Apple targets).
    const auto significand = std::string_view(token).substr(0, token.find_first_of("eE"));
    const bool nonzero = significand.find_first_of("123456789") != std::string_view::npos;
    if (!std::isfinite(value) || (value == 0.0 && nonzero))
      return reject("nonfinite or out-of-range Double");
    return add(value);
  }
  bool string(string_t &value) override {
    if (value.size() > kMaxStringBytes)
      return reject("string too large");
    return add(std::move(value));
  }
  bool binary(binary_t &) override { return reject("binary JSON value unsupported"); }
  bool start_object(std::size_t) override {
    if (started)
      return reject("nested properties unsupported");
    started = true;
    return true;
  }
  bool key(string_t &key) override {
    if (key.size() > kMaxPropertyKeyBytes || values.size() >= kMaxProperties)
      return reject("property limit exceeded");
    if (values.count(key))
      return reject("duplicate property key");
    current = std::move(key);
    pending = true;
    return true;
  }
  bool end_object() override {
    complete = true;
    return true;
  }
  bool start_array(std::size_t) override { return reject("array properties unsupported"); }
  bool end_array() override { return reject("array properties unsupported"); }
  bool parse_error(std::size_t, const std::string &,
                   const nlohmann::detail::exception &exception) override {
    error = exception.what();
    return false;
  }
};
} // namespace
PropertyMap parse_json_properties(std::string_view json) {
  // JSON must escape NUL; the SAX lexer otherwise treats a raw NUL as EOF.
  if (json.find('\0') != std::string_view::npos)
    throw std::invalid_argument("parameter JSON contains a raw NUL byte");
  if (json.size() > kMaxStringBytes)
    throw std::invalid_argument("parameter JSON exceeds 1 MiB");
  FlatSax parser;
  if (!Json::sax_parse(json.begin(), json.end(), &parser) || !parser.complete)
    throw std::invalid_argument("invalid parameter JSON: " + parser.error);
  return std::move(parser.values);
}
std::string json_escape(std::string_view value) {
  auto encoded = Json(std::string(value)).dump();
  return encoded.substr(1, encoded.size() - 2);
}
std::string property_to_string(const Property &value) {
  if (auto text = std::get_if<std::string>(&value))
    return *text;
  return property_to_json(value);
}
std::string property_to_json(const Property &value) {
  return std::visit(
      [](const auto &v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          return "null";
        else {
          if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(v))
              throw std::invalid_argument("nonfinite Double");
          }
          if constexpr (std::is_same_v<T, std::string>) {
            if (v.size() > kMaxStringBytes)
              throw std::invalid_argument("string exceeds 1 MiB");
          }
          return Json(v).dump();
        }
      },
      value);
}
void validate_identity(std::string_view value) {
  if (value.empty() || value.size() > kMaxIdentityBytes)
    throw std::invalid_argument("identity must contain 1..1024 UTF-8 bytes");
  (void)json_escape(value);
}
void validate_properties(const PropertyMap &values) {
  if (values.size() > kMaxProperties)
    throw std::invalid_argument("too many properties");
  size_t total = 0;
  for (const auto &[key, value] : values) {
    if (key.size() > kMaxPropertyKeyBytes)
      throw std::invalid_argument("property key too large");
    total += json_escape(key).size() + property_to_json(value).size() + 8;
    if (total > kMaxObjectBytes)
      throw std::invalid_argument("object exceeds 16 MiB");
  }
}
std::string tuple_key(std::initializer_list<std::string_view> parts) {
  std::string key;
  for (auto part : parts) {
    key += std::to_string(part.size());
    key += ':';
    key.append(part);
  }
  return key;
}
std::string edge_id(std::string_view from, std::string_view type, std::string_view to) {
  validate_identity(from);
  validate_identity(type);
  validate_identity(to);
  return tuple_key({from, type, to});
}
std::string property_equality_key(const Property &value) {
  if (auto integer = std::get_if<int64_t>(&value))
    return "n" + std::to_string(*integer);
  if (auto real = std::get_if<double>(&value)) {
    if (!std::isfinite(*real))
      throw std::invalid_argument("nonfinite index value");
    if (*real >= -9223372036854775808.0 && *real < 9223372036854775808.0 &&
        std::trunc(*real) == *real)
      return "n" + std::to_string(static_cast<int64_t>(*real));
    return "d" + property_to_json(value);
  }
  return std::to_string(value.index()) + property_to_json(value);
}
} // namespace graphdb
