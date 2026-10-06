#include "graphdb/QueryContext.hpp"
namespace graphdb {
Status QueryContext::check() const {
  auto validated=options_.validate(); if(!validated.ok)return validated;
  if (cancelled_.load() || (request_ && request_->cancelled.load()))
    return Status::Error("query cancelled", ErrorCode::cancelled);
  if (std::chrono::steady_clock::now() >= deadline_)
    return Status::Error("query deadline exceeded", ErrorCode::deadlineExceeded);
  return Status::OK();
}
} // namespace graphdb
