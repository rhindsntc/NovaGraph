#pragma once
#include "graphdb/Status.hpp"
#include <cstddef>
#include <cstdint>
namespace graphdb {
// Safety ceilings. Applications may lower these, never disable them with zero.
struct QueryOptions {
  size_t max_depth{16}, max_results{10000}, max_expanded_edges{100000};
  size_t working_bytes{16 * 1024 * 1024}, result_bytes{16 * 1024 * 1024};
  size_t max_statements{1000}, batch_bytes{16 * 1024 * 1024};
  int64_t timeout_ms{5000};
  Status validate() const {
    if(max_depth>16 || max_results>10000 || max_expanded_edges>100000 ||
       !working_bytes || working_bytes>16*1024*1024 || !result_bytes || result_bytes>16*1024*1024 ||
       !max_statements || max_statements>1000 || !batch_bytes || batch_bytes>16*1024*1024 ||
       timeout_ms<=0 || timeout_ms>5000)
      return Status::Error("query options must be within the supported safety ceilings",ErrorCode::invalidArgument);
    return Status::OK();
  }
};
} // namespace graphdb
