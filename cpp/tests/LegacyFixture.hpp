#pragma once
#include "TestSupport.hpp"
namespace nova_test {
inline std::vector<uint8_t> legacy_record(graphdb::GraphObject o) {
  using namespace graphdb;
  std::vector<uint8_t> b;
  encode_u32(b, 0x4E4F5641);
  encode_u32(b, 1);
  encode_u8(b, uint8_t(o.kind));
  encode_i64(b, o.last_read_ms);
  encode_i64(b, o.last_modified_ms);
  encode_u64(b, o.version);
  encode_string(b, o.id);
  encode_string(b, o.label_or_type);
  encode_string(b, o.from);
  encode_string(b, o.to);
  encode_properties(b, o.properties);
  encode_u32(b, crc32_compute(b.data(), b.size()));
  return b;
}
inline void legacy_fixture(const std::filesystem::path &dir) {
  using namespace graphdb;
  std::filesystem::create_directories(dir / "cold");
  auto io = default_file_io();
  GraphObject a;
  a.id = "a";
  a.label_or_type = "N";
  a.version = 1;
  a.last_modified_ms = 123;
  a.properties = {
      {"v", int64_t(7)},         {"large", INT64_MAX}, {"zero", int64_t(0)},         {"b", true},
      {"nil", std::monostate{}}, {"d", 1.5},           {"s", std::string("a\0b", 3)}};
  auto payload = legacy_record(a);
  CHECK(io->write_all(dir / "cold" / "a.rec", payload, WriteMode::exclusive).ok);
  a.properties.clear();
  a.version = 2;
  GraphObject b;
  b.id = "b";
  b.label_or_type = "N";
  b.version = 1;
  b.properties = {{"v", int64_t(8)}};
  GraphObject e;
  e.kind = ObjectKind::Edge;
  e.id = "a->E->b";
  e.from = "a";
  e.to = "b";
  e.label_or_type = "E";
  e.version = 1;
  e.properties = {{"v", int64_t(9)}};
  std::vector<uint8_t> cat;
  encode_u32(cat, 0x4E434154);
  encode_u32(cat, 1);
  encode_u64(cat, 0);
  encode_u32(cat, 1);
  encode_string(cat, "N");
  encode_string(cat, "v");
  encode_u32(cat, 3);
  for (const auto &obj : {a, b, e}) {
    encode_string(cat, object_key(obj.kind, obj.id));
    encode_u8(cat, obj.id == "a" ? 2 : 1);
    encode_string(cat, obj.id == "a" ? "a.rec" : "");
    auto rec = legacy_record(obj);
    encode_u32(cat, rec.size());
    cat.insert(cat.end(), rec.begin(), rec.end());
  }
  encode_u32(cat, crc32_compute(cat.data(), cat.size()));
  CHECK(io->write_all(dir / "catalog.db", cat, WriteMode::exclusive).ok);
  std::vector<uint8_t> wal;
  encode_u32(wal, 0x4E57414C);
  encode_u32(wal, 1);
  std::vector<uint8_t> frame;
  encode_u64(frame, 1);
  encode_u8(frame, 1);
  encode_string(frame, "N");
  encode_string(frame, "c");
  encode_properties(frame, {{"v", int64_t(10)}});
  encode_u32(wal, frame.size());
  wal.insert(wal.end(), frame.begin(), frame.end());
  encode_u32(wal, crc32_compute(frame.data(), frame.size()));
  CHECK(io->write_all(dir / "wal.log", wal, WriteMode::exclusive).ok);
}
} // namespace nova_test
