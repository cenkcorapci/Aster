#pragma once

#include <string>
#include <utility>
#include <variant>

namespace aster {

// Aster avoids exceptions in hot paths (see docs/code-structure.md).
// All fallible operations return Status or Result<T>.

enum class StatusCode {
  kOk = 0,
  kNotFound,
  kInvalidArgument,
  kIoError,
  kCorruption,
  kUnavailable,
  kResourceExhausted,
  kInternal,
};

class Status {
 public:
  Status() : code_(StatusCode::kOk) {}
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Status Ok() { return Status(); }
  static Status NotFound(std::string m = "") {
    return Status(StatusCode::kNotFound, std::move(m));
  }
  static Status InvalidArgument(std::string m = "") {
    return Status(StatusCode::kInvalidArgument, std::move(m));
  }
  static Status IoError(std::string m = "") {
    return Status(StatusCode::kIoError, std::move(m));
  }
  static Status Corruption(std::string m = "") {
    return Status(StatusCode::kCorruption, std::move(m));
  }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

 private:
  StatusCode code_;
  std::string message_;
};

template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT
  Result(Status status) : value_(std::move(status)) {}   // NOLINT

  bool ok() const { return std::holds_alternative<T>(value_); }
  const T& value() const { return std::get<T>(value_); }
  T& value() { return std::get<T>(value_); }
  const Status& status() const { return std::get<Status>(value_); }

 private:
  std::variant<T, Status> value_;
};

}  // namespace aster
