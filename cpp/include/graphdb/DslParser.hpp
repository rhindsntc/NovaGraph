#pragma once

#include "graphdb/GraphEngine.hpp"

namespace graphdb {

QueryResult execute_ngql_unlocked(GraphEngine &, std::string, const PropertyMap &);
// Executes the embedded NovaGraph Query Language (NGQL).
QueryResult execute_ngql(GraphEngine &engine, std::string query,
                         const PropertyMap &parameters = {});
PropertyMap parse_property_list(std::string input);

} // namespace graphdb
