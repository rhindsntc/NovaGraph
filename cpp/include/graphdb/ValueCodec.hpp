#pragma once
#include "graphdb/GraphTypes.hpp"
namespace graphdb {
constexpr size_t kMaxIdentityBytes = 1024, kMaxPropertyKeyBytes = 1024;
constexpr size_t kMaxStringBytes = 1024 * 1024, kMaxProperties = 4096,
                 kMaxObjectBytes = 16 * 1024 * 1024;
PropertyMap parse_json_properties(std::string_view json);
std::string property_equality_key(const Property &value);
std::string tuple_key(std::initializer_list<std::string_view> parts);
void validate_properties(const PropertyMap &values);
void validate_identity(std::string_view value);
} // namespace graphdb
