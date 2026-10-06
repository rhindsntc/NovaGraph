#pragma once
#include <string>
#include <optional>
#include <utility>
namespace graphdb {
enum class ErrorCode {
  ok,
  notFound,
  busy,
  invalidArgument,
  parseError,
  unboundParameter,
  limitExceeded,
  cancelled,
  deadlineExceeded,
  ioFailure,
  corruptData,
  unsupportedVersion,
  closed,
  conflict,
  commitOutcomeUnknown
};
inline const char *error_code_name(ErrorCode code) {
  switch (code) {
#define NOVA_ERROR(name)                                                                           \
  case ErrorCode::name:                                                                            \
    return #name;
    NOVA_ERROR(ok)
    NOVA_ERROR(notFound) NOVA_ERROR(busy) NOVA_ERROR(invalidArgument) NOVA_ERROR(parseError)
        NOVA_ERROR(unboundParameter) NOVA_ERROR(limitExceeded) NOVA_ERROR(cancelled)
            NOVA_ERROR(deadlineExceeded) NOVA_ERROR(ioFailure) NOVA_ERROR(corruptData)
                NOVA_ERROR(unsupportedVersion) NOVA_ERROR(closed) NOVA_ERROR(conflict)
                    NOVA_ERROR(commitOutcomeUnknown)
#undef NOVA_ERROR
  }
  return "ioFailure";
}
struct ErrorContext {
  std::string file;
  int os_error{0};
  std::string object;
  std::string transaction_id;
  std::optional<size_t> statement_index, source_start, source_end;
  ErrorContext(std::string file = {}, int os_error = 0, std::string object = {},
               std::string transaction_id = {})
      : file(std::move(file)), os_error(os_error), object(std::move(object)),
        transaction_id(std::move(transaction_id)) {}
};
struct Status {
  bool ok{true};
  std::string message;
  ErrorCode code{ErrorCode::ok};
  ErrorContext context;
  static Status OK() { return {}; }
  static Status Error(std::string message, ErrorCode code = ErrorCode::ioFailure,
                      ErrorContext context = {}) {
    return {false, std::move(message), code, std::move(context)};
  }
};
template <class T> struct Result {
  Status status;
  T value{};
  explicit operator bool() const { return status.ok; }
};
} // namespace graphdb
