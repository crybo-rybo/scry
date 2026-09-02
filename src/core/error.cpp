/// @file
/// @brief Implements the common internal `Error` construction path.
///
/// Centralizing scalar initialization keeps every subsystem on the same
/// value-based failure contract while leaving correlation metadata to the
/// layer that owns it.

#include "core/error.hpp"

#include <utility>

namespace scry::detail {

Error make_error(const ErrorCategory category, std::string message,
                 const bool retryable) {
  return Error{
      .category = category,
      .retryable = retryable,
      .message = std::move(message),
  };
}

} // namespace scry::detail
