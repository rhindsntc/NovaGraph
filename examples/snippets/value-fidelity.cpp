#include "graphdb/BinaryEncoding.hpp"
#include "graphdb/ValueCodec.hpp"
#include <stdexcept>

// Executed by cpp.documentation_value_fidelity_example.
void value_fidelity_example() {
  auto values = graphdb::parse_json_properties(
      R"({"large":9007199254740993,"score":1.23456789,"text":"a=b,\n\u0000雪","active":true,"none":null})");
  graphdb::GraphObject node;
  node.id = "sample";
  node.label_or_type = "Example";
  node.properties = values;
  auto bytes = graphdb::serialize_object_binary(node);
  auto decoded = graphdb::deserialize_object_binary(bytes.data(), bytes.size());
  if (!decoded || decoded.value.properties != values ||
      graphdb::parse_json_properties(graphdb::properties_to_json(values)) != values) {
    throw std::runtime_error("scalar fidelity failed");
  }
}
