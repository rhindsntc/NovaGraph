#pragma once
#include <cstddef>
namespace graphdb {
struct MemoryLimits {
  size_t hot_payload_bytes{64 * 1024 * 1024}, metadata_bytes{64 * 1024 * 1024},
      query_bytes{64 * 1024 * 1024}, result_bytes{16 * 1024 * 1024};
};
struct MemoryUsage {
  size_t hot_payload_bytes{0}, metadata_bytes{0}, query_bytes{0}, result_bytes{0};
};
struct TrimResult {
  size_t evicted_count{0}, evicted_bytes{0}, pinned_bytes{0}, unmet_bytes{0};
};
struct GarbageResult {
  size_t inspected{0}, removed{0};
};
} // namespace graphdb
