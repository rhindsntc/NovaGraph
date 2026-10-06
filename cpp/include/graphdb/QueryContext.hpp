#pragma once
#include "graphdb/QueryOptions.hpp"
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
namespace graphdb {
class QueryFailure : public std::runtime_error {
public:
  Status status;
  explicit QueryFailure(Status s) : std::runtime_error(s.message), status(std::move(s)) {}
};
struct RequestState {
  std::atomic<bool> cancelled{false};
  const std::chrono::steady_clock::time_point deadline;
  explicit RequestState(int64_t timeout_ms)
      : deadline(std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms)) {}
};
// Reservations can outlive the request when immutable staged nodes are published.
// Their counter owns no engine state; releasing old nodes never touches a dead context.
class WorkReservation {
  std::shared_ptr<std::atomic<size_t>> counter_;
  size_t bytes_{0};
public:
  WorkReservation() = default;
  WorkReservation(std::shared_ptr<std::atomic<size_t>> counter, size_t bytes)
      : counter_(std::move(counter)), bytes_(bytes) {}
  WorkReservation(const WorkReservation &) = delete;
  WorkReservation &operator=(const WorkReservation &) = delete;
  WorkReservation(WorkReservation &&other) noexcept
      : counter_(std::move(other.counter_)), bytes_(other.bytes_) {}
  ~WorkReservation() { if (counter_) counter_->fetch_sub(bytes_, std::memory_order_relaxed); }
};
class QueryContext {
  std::atomic<bool> cancelled_{false};
  std::chrono::steady_clock::time_point deadline_;
  QueryOptions options_;
  std::shared_ptr<RequestState> request_;
  std::shared_ptr<std::atomic<size_t>> working_used_=std::make_shared<std::atomic<size_t>>(0);
  size_t working_peak_{0}, result_used_{0}, edges_{0};

public:
  // v2 transport tags property scalars; legacy/native JSON stays unchanged.
  bool tagged_values{false};
  bool read_only{false};
  explicit QueryContext(
      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max())
      : deadline_(deadline) {}
  explicit QueryContext(QueryOptions options)
      : deadline_(std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeout_ms>0 && options.timeout_ms<=5000 ? options.timeout_ms : 0)), options_(options) {}
  const QueryOptions &options() const { return options_; }
  void enforce() const { auto s=check(); if(!s.ok) throw QueryFailure(s); }
  // Reserved workspace bytes, not allocator usage. The peak survives lease release
  // and can exceed a subsequently tightened working_bytes limit.
  size_t working_used() const { return working_used_->load(std::memory_order_relaxed); }
  size_t working_peak() const { return working_peak_; }
  size_t working_available() const {
    auto used=working_used();
    return used>=options_.working_bytes?0:options_.working_bytes-used;
  }
  void reserve_work(size_t bytes) {
    enforce();
    if(bytes>working_available()) throw QueryFailure(Status::Error("query working memory budget exceeded",ErrorCode::limitExceeded));
    auto used=working_used_->fetch_add(bytes,std::memory_order_relaxed)+bytes;
    working_peak_=std::max(working_peak_,used);
  }
  WorkReservation hold_work(size_t bytes) {
    reserve_work(bytes);
    return WorkReservation(working_used_,bytes);
  }
  void reserve_result(size_t bytes) {
    enforce();
    if(bytes>options_.result_bytes-result_used_) throw QueryFailure(Status::Error("serialized result budget exceeded",ErrorCode::limitExceeded));
    result_used_+=bytes;
  }
  void expand_edge() {
    enforce();
    if(edges_>=options_.max_expanded_edges) throw QueryFailure(Status::Error("expanded edge budget exceeded",ErrorCode::limitExceeded));
    ++edges_;
  }
  void bounds(size_t depth, size_t results) const {
    enforce();
    if(depth>options_.max_depth || results>options_.max_results) throw QueryFailure(Status::Error("query depth/result limit exceeds configured ceiling",ErrorCode::limitExceeded));
  }
  void tighten(const QueryOptions &ceiling) {
    enforce(); auto valid=ceiling.validate(); if(!valid.ok)throw QueryFailure(valid);
    options_.max_depth=std::min(options_.max_depth,ceiling.max_depth);
    options_.max_results=std::min(options_.max_results,ceiling.max_results);
    options_.max_expanded_edges=std::min(options_.max_expanded_edges,ceiling.max_expanded_edges);
    options_.max_statements=std::min(options_.max_statements,ceiling.max_statements);
    options_.working_bytes=std::min(options_.working_bytes,ceiling.working_bytes);
    options_.result_bytes=std::min(options_.result_bytes,ceiling.result_bytes);
    options_.batch_bytes=std::min(options_.batch_bytes,ceiling.batch_bytes);
    options_.timeout_ms=std::min(options_.timeout_ms,ceiling.timeout_ms);
    deadline_=std::min(deadline_,std::chrono::steady_clock::now()+std::chrono::milliseconds(options_.timeout_ms));
    if(working_used_->load(std::memory_order_relaxed)>options_.working_bytes || result_used_>options_.result_bytes)
      throw QueryFailure(Status::Error("query context already exceeds database budget",ErrorCode::limitExceeded));
  }
  void use_request(std::shared_ptr<RequestState> request) noexcept {
    request_=std::move(request);
    if(request_)deadline_=std::min(deadline_,request_->deadline);
  }
  void cancel() noexcept { cancelled_.store(true); }
  Status check() const;
};
} // namespace graphdb
