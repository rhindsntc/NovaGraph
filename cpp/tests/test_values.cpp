#include "TestSupport.hpp"
#include "graphdb/BinaryEncoding.hpp"
using namespace graphdb;
NOVA_TEST(binary_round_trip, "values", "") {
  GraphObject obj;
  obj.kind = ObjectKind::Node;
  obj.id = "user:special;1=";
  obj.label_or_type = "Person";
  obj.version = 42;
  obj.properties = {{"bio", std::string("Line 1\nLine 2; key=value")},
                    {"age", int64_t(30)},
                    {"score", 99.5},
                    {"active", true},
                    {"none", std::monostate{}}};
  auto bytes = serialize_object_binary(obj);
  auto result = deserialize_object_binary(bytes.data(), bytes.size());
  CHECK(result);
  CHECK(result.value.id == "user:special;1=");
  CHECK(result.value.properties == obj.properties);
  CHECK(result.value.version == 42);
}
NOVA_TEST(binary_corruption_detected, "values", "") {
  GraphObject obj;
  obj.id = "one";
  auto bytes = serialize_object_binary(obj);
  bytes.at(bytes.size() - 5) ^= 0xff;
  auto result = deserialize_object_binary(bytes.data(), bytes.size());
  CHECK(!result);
}
#include "graphdb/DslParser.hpp"
#include <cmath>
#include <limits>
using namespace nova_test;
NOVA_TEST(json_scalar_round_trips, "values", "") {
  std::vector<Property> values = {std::monostate{},
                                  false,
                                  true,
                                  int64_t(0),
                                  int64_t(1),
                                  INT64_MIN,
                                  INT64_MAX,
                                  int64_t(9007199254740993LL),
                                  -0.0,
                                  1.0,
                                  1.23456789,
                                  std::string("Unicode: π 雪 \\"),
                                  std::string("comma,equals=\n\t\""),
                                  std::string("a\0b", 3)};
  for (const auto &value : values) {
    auto json = properties_to_json({{"v", value}});
    auto decoded = parse_property_list(json);
    CHECK(decoded.at("v").index() == value.index());
    CHECK(decoded.at("v") == value);
    if (std::holds_alternative<double>(value))
      CHECK(std::signbit(std::get<double>(decoded.at("v"))) ==
            std::signbit(std::get<double>(value)));
  }
}
NOVA_TEST(json_rejects_malformed_and_unsupported_values, "values", "") {
  for (const auto &json : std::vector<std::string>{
           "{", "{\"x\":1,}", "{\"x\":1,\"x\":2}", "{\"x\":[]}", "{\"x\":{}}",
           "{\"x\":9223372036854775808}", "{\"x\":18446744073709551616}", "{\"x\":1e999}",
           "{\"x\":NaN}", "{\"x\":\"\\ud800\"}", std::string(1024 * 1024 + 1, ' ')}) {
    bool rejected = false;
    try {
      parse_property_list(json);
    } catch (const std::exception &) {
      rejected = true;
    }
    CHECK(rejected);
  }
}
NOVA_TEST(nonfinite_values_are_rejected, "values", "") {
  for (double value : {INFINITY, -INFINITY, NAN}) {
    bool rejected = false;
    try {
      property_to_json(Property{value});
    } catch (const std::exception &) {
      rejected = true;
    }
    CHECK(rejected);
    GraphObject object;
    object.id = "n";
    object.properties["v"] = value;
    rejected = false;
    try {
      serialize_object_binary(object);
    } catch (const std::exception &) {
      rejected = true;
    }
    CHECK(rejected);
  }
}
NOVA_TEST(numeric_comparisons_are_exact, "values", "") {
  CHECK(!compare_properties(Property{int64_t(9007199254740993LL)}, "=",
                            Property{9007199254740992.0}));
  CHECK(compare_properties(Property{INT64_MAX}, "<", Property{9223372036854775808.0}));
  CHECK(compare_properties(Property{INT64_MIN}, "=", Property{-9223372036854775808.0}));
  CHECK(compare_properties(Property{int64_t(-1)}, ">", Property{-1.5}));
  CHECK(compare_properties(Property{-0.0}, "=", Property{int64_t(0)}));
  CHECK(!compare_properties(Property{true}, "=", Property{int64_t(1)}));
}
NOVA_TEST(indexed_and_scanned_numeric_equality_agree, "values", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N", "a", {{"v", int64_t(1)}}).ok);
  CHECK(f.engine->upsert_node("N", "b", {{"v", 1.0}}).ok);
  auto scan = f.engine->find_nodes("N", "v", Property{1.0}, 10);
  CHECK(scan);
  CHECK(scan.value.size() == 2);
  CHECK(f.engine->create_node_property_index("N", "v").ok);
  auto indexed = f.engine->find_nodes("N", "v", Property{int64_t(1)}, 10);
  CHECK(indexed);
  CHECK(indexed.value.size() == scan.value.size());
}
NOVA_TEST(edge_and_index_tuples_do_not_alias, "values", "") {
  CHECK(edge_id("a->b", "c", "d") != edge_id("a", "b->c", "d"));
  EngineFixture f;
  CHECK(f.engine
            ->upsert_node("a\x1f"
                          "b",
                          "one", {{"c", int64_t(1)}})
            .ok);
  CHECK(f.engine
            ->upsert_node("a", "two",
                          {{"b\x1f"
                            "c",
                            int64_t(1)}})
            .ok);
  CHECK(f.engine
            ->create_node_property_index("a\x1f"
                                         "b",
                                         "c")
            .ok);
  auto found = f.engine->find_nodes("a\x1f"
                                    "b",
                                    "c", int64_t(1), 10);
  CHECK(found);
  CHECK(found.value.size() == 1);
  CHECK(found.value[0].id == "one");
}
NOVA_TEST(identity_limits_and_bounded_payload_names, "values", "") {
  EngineFixture f;
  CHECK(!f.engine->upsert_node("N", std::string(1025, 'x'), {}).ok);
  auto id = std::string(1024, 'x');
  CHECK(f.engine->upsert_node("N", id, {}).ok);
  f.cold("n/" + id);
  CHECK(f.hot->cold_count() == 1);
  for (auto &entry : std::filesystem::directory_iterator(f.dir.path() / "cold"))
    CHECK(entry.path().filename().string().size() < 128);
}
NOVA_TEST(binary_decoder_rejects_invalid_kind_and_trailing_bytes, "values", "") {
  GraphObject obj;
  obj.id = "x";
  auto good = serialize_object_binary(obj);
  for (bool trailing : {false, true}) {
    auto data = good;
    data.resize(data.size() - 4);
    if (trailing)
      data.push_back(0);
    else
      data[8] = 99;
    auto crc = crc32_compute(data.data(), data.size());
    encode_u32(data, crc);
    CHECK(!deserialize_object_binary(data.data(), data.size()));
  }
}
NOVA_TEST(c_parameter_errors_stay_inside_c_boundary, "values", "") {
  TempDirectory dir;
  Handle db(dir.path());
  auto result = db.query("upsert node N a set x=$x", "{\"x\":[]}");
  CHECK(result.find("\"ok\":false") != std::string::npos);
}
NOVA_TEST(binary_scalar_types_and_identity_validation, "values", "") {
  GraphObject object;
  object.id = std::string("a\0b", 3);
  object.label_or_type = "N";
  object.properties = {{"low", INT64_MIN},
                       {"high", INT64_MAX},
                       {"precise", 1.23456789},
                       {"zero", -0.0},
                       {"nul", std::string("a\0b", 3)},
                       {"null", std::monostate{}},
                       {"bool", true}};
  auto bytes = serialize_object_binary(object);
  auto result = deserialize_object_binary(bytes.data(), bytes.size());
  CHECK(result);
  CHECK(result.value.properties == object.properties);
  CHECK(std::signbit(std::get<double>(result.value.properties.at("zero"))));
  object.id = std::string(1025, 'x');
  bytes = serialize_object_binary(object);
  CHECK(!deserialize_object_binary(bytes.data(), bytes.size()));
}
#include "../../examples/snippets/value-fidelity.cpp"
NOVA_TEST(documentation_value_fidelity_example, "values", "") { value_fidelity_example(); }
NOVA_TEST(numeric_parameter_identities_keep_precision, "values", "") {
  CHECK(property_to_string(Property{1.23456789}) == "1.23456789");
  EngineFixture f;
  CHECK(f.engine->execute_dsl("upsert node N $id", {{"id", 1.23456789}}).ok);
  CHECK(f.engine->execute_dsl("upsert node N $id", {{"id", 1.23456788}}).ok);
  CHECK(f.engine->get_node("1.23456789"));
  CHECK(f.engine->get_node("1.23456788"));
}

NOVA_TEST(json_double_extremes_preserve_representable_values_and_reject_underflow, "values", "") {
  auto values = parse_property_list(R"({"tiny":5e-324,"max":1.7976931348623157e308,"zero":0e-999,"negativeZero":-0.0})");
  CHECK(std::get<double>(values.at("tiny")) == std::numeric_limits<double>::denorm_min());
  CHECK(std::get<double>(values.at("max")) == std::numeric_limits<double>::max());
  CHECK(std::get<double>(values.at("zero")) == 0.0);
  CHECK(std::signbit(std::get<double>(values.at("negativeZero"))));
  for (const auto &token : {"1e-999", "-1e-999", "0.0001e-999", "1e999", "-1e999"}) {
    bool rejected = false;
    try { parse_property_list(std::string("{\"value\":") + token + "}"); }
    catch (const std::invalid_argument &) { rejected = true; }
    CHECK(rejected);
  }
}
