#pragma once

#include "graphdb/GraphTypes.hpp"
#include "graphdb/ValueCodec.hpp"
#include <algorithm>
#include <cmath>
#include <span>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace graphdb {

inline uint32_t crc32_compute(const uint8_t *data, size_t length, uint32_t prev_crc = 0) {
  uint32_t crc = ~prev_crc;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320u * (crc & 1));
    }
  }
  return ~crc;
}

inline void encode_u8(std::vector<uint8_t> &buf, uint8_t v) { buf.push_back(v); }

inline void encode_u32(std::vector<uint8_t> &buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

inline void encode_u64(std::vector<uint8_t> &buf, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
  }
}

inline void encode_i64(std::vector<uint8_t> &buf, int64_t v) {
  encode_u64(buf, static_cast<uint64_t>(v));
}

inline void encode_double(std::vector<uint8_t> &buf, double v) {
  uint64_t raw;
  std::memcpy(&raw, &v, sizeof(v));
  encode_u64(buf, raw);
}

inline void encode_string(std::vector<uint8_t> &buf, std::string_view s) {
  encode_u32(buf, static_cast<uint32_t>(s.size()));
  buf.insert(buf.end(), reinterpret_cast<const uint8_t *>(s.data()),
             reinterpret_cast<const uint8_t *>(s.data() + s.size()));
}

inline void encode_property(std::vector<uint8_t> &buf, const Property &prop) {
  if (std::holds_alternative<std::monostate>(prop)) {
    encode_u8(buf, 0);
  } else if (std::holds_alternative<bool>(prop)) {
    encode_u8(buf, 1);
    encode_u8(buf, std::get<bool>(prop) ? 1 : 0);
  } else if (std::holds_alternative<int64_t>(prop)) {
    encode_u8(buf, 2);
    encode_i64(buf, std::get<int64_t>(prop));
  } else if (std::holds_alternative<double>(prop)) {
    encode_u8(buf, 3);
    encode_double(buf, std::get<double>(prop));
  } else if (std::holds_alternative<std::string>(prop)) {
    encode_u8(buf, 4);
    encode_string(buf, std::get<std::string>(prop));
  }
}

inline void encode_properties(std::vector<uint8_t> &buf, const PropertyMap &props) {
  encode_u32(buf, static_cast<uint32_t>(props.size()));
  std::vector<std::string> keys;
  keys.reserve(props.size());
  for (const auto &[k, v] : props) {
    (void)v;
    keys.push_back(k);
  }
  std::sort(keys.begin(), keys.end());
  for (const auto &key : keys) {
    encode_string(buf, key);
    encode_property(buf, props.at(key));
  }
}

class ByteReader {
public:
  ByteReader(const uint8_t *data, size_t size) : data_(data), size_(size), pos_(0) {}

  bool has_bytes(size_t n) const { return n <= size_ - pos_; }
  size_t remaining() const { return size_ - pos_; }
  size_t position() const { return pos_; }

  bool skip(size_t n) {
    if (!has_bytes(n))
      return false;
    pos_ += n;
    return true;
  }

  bool read_u8(uint8_t &v) {
    if (!has_bytes(1))
      return false;
    v = data_[pos_++];
    return true;
  }

  bool read_u32(uint32_t &v) {
    if (!has_bytes(4))
      return false;
    v = static_cast<uint32_t>(data_[pos_]) | (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
        (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
        (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return true;
  }

  bool read_u64(uint64_t &v) {
    if (!has_bytes(8))
      return false;
    v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= (static_cast<uint64_t>(data_[pos_ + i]) << (i * 8));
    }
    pos_ += 8;
    return true;
  }

  bool read_i64(int64_t &v) {
    uint64_t raw;
    if (!read_u64(raw))
      return false;
    v = static_cast<int64_t>(raw);
    return true;
  }

  bool read_double(double &v) {
    uint64_t raw;
    if (!read_u64(raw))
      return false;
    std::memcpy(&v, &raw, sizeof(v));
    return true;
  }

  bool read_string(std::string &s, size_t maximum = kMaxStringBytes) {
    uint32_t len;
    if (!read_u32(len))
      return false;
    if (len > maximum || !has_bytes(len))
      return false;
    s.assign(reinterpret_cast<const char *>(data_ + pos_), len);
    pos_ += len;
    return true;
  }

  bool read_property(Property &prop) {
    uint8_t type;
    if (!read_u8(type))
      return false;
    switch (type) {
    case 0:
      prop = std::monostate{};
      return true;
    case 1: {
      uint8_t b;
      if (!read_u8(b) || b > 1)
        return false;
      prop = (b != 0);
      return true;
    }
    case 2: {
      int64_t i;
      if (!read_i64(i))
        return false;
      prop = i;
      return true;
    }
    case 3: {
      double d;
      if (!read_double(d) || !std::isfinite(d))
        return false;
      prop = d;
      return true;
    }
    case 4: {
      std::string s;
      if (!read_string(s))
        return false;
      prop = std::move(s);
      return true;
    }
    default:
      return false;
    }
  }

  bool read_properties(PropertyMap &props) {
    uint32_t count;
    if (!read_u32(count) || count > kMaxProperties || count > remaining() / 5)
      return false;
    props.clear();
    for (uint32_t i = 0; i < count; ++i) {
      std::string k;
      if (!read_string(k) || k.size() > kMaxPropertyKeyBytes || props.count(k))
        return false;
      Property v;
      if (!read_property(v))
        return false;
      props[std::move(k)] = std::move(v);
    }
    return true;
  }

private:
  const uint8_t *data_;
  size_t size_;
  size_t pos_;
};

constexpr uint32_t kNovaMagic = 0x4E4F5641, kFormatVersion = 3;
inline std::vector<uint8_t> encode_envelope(uint32_t magic, const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> bytes;
  encode_u32(bytes, magic);
  encode_u32(bytes, kFormatVersion);
  encode_u64(bytes, payload.size());
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  encode_u32(bytes, crc32_compute(bytes.data(), bytes.size()));
  return bytes;
}
inline Result<std::span<const uint8_t>> decode_envelope(const uint8_t *data, size_t size,
                                                        uint32_t magic, size_t maximum) {
  if (size > maximum || size < 20)
    return {Status::Error("invalid file extent", ErrorCode::corruptData), {}};
  ByteReader r(data, size);
  uint32_t m, v, crc;
  uint64_t extent;
  r.read_u32(m);
  r.read_u32(v);
  r.read_u64(extent);
  if (m != magic)
    return {Status::Error("invalid file magic", ErrorCode::corruptData), {}};
  if (v != kFormatVersion)
    return {Status::Error("unsupported file version", ErrorCode::unsupportedVersion), {}};
  if (extent != size - 20)
    return {Status::Error("invalid payload extent", ErrorCode::corruptData), {}};
  r.skip(size - 20);
  r.read_u32(crc);
  if (crc != crc32_compute(data, size - 4))
    return {Status::Error("checksum mismatch", ErrorCode::corruptData), {}};
  return {Status::OK(), std::span<const uint8_t>(data + 16, size - 20)};
}
inline std::vector<uint8_t> serialize_object_binary(const GraphObject &obj) {
  validate_properties(obj.properties);
  std::vector<uint8_t> p;
  encode_u8(p, uint8_t(obj.kind));
  encode_string(p, obj.id);
  encode_string(p, obj.label_or_type);
  encode_string(p, obj.from);
  encode_string(p, obj.to);
  encode_u64(p, obj.version);
  encode_i64(p, obj.last_modified_ms);
  encode_properties(p, obj.properties);
  if (p.size() + 20 > kMaxObjectBytes)
    throw std::invalid_argument("object exceeds 16 MiB");
  return encode_envelope(kNovaMagic, p);
}
inline Result<GraphObject> deserialize_object_binary(const uint8_t *data, size_t size) {
  auto decoded = decode_envelope(data, size, kNovaMagic, kMaxObjectBytes);
  if (!decoded)
    return {decoded.status, {}};
  ByteReader r(decoded.value.data(), decoded.value.size());
  GraphObject obj;
  uint8_t kind;
  if (!r.read_u8(kind) || (kind != 1 && kind != 2) || !r.read_string(obj.id) ||
      !r.read_string(obj.label_or_type) || !r.read_string(obj.from) || !r.read_string(obj.to) ||
      !r.read_u64(obj.version) || !r.read_i64(obj.last_modified_ms) ||
      !r.read_properties(obj.properties) || r.remaining())
    return {Status::Error("invalid record encoding", ErrorCode::corruptData), {}};
  obj.kind = static_cast<ObjectKind>(kind);
  obj.last_read_ms = obj.last_modified_ms;
  try {
    validate_identity(obj.label_or_type);
    validate_properties(obj.properties);
    if (obj.kind == ObjectKind::Node) {
      validate_identity(obj.id);
      if (!obj.from.empty() || !obj.to.empty())
        throw std::invalid_argument("node has endpoints");
    } else if (obj.id != edge_id(obj.from, obj.label_or_type, obj.to))
      throw std::invalid_argument("edge identity mismatch");
  } catch (const std::exception &e) {
    return {Status::Error(e.what(), ErrorCode::corruptData), {}};
  }
  return {Status::OK(), std::move(obj)};
}
} // namespace graphdb
