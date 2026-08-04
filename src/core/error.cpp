#include "core/error.hpp"

#include <utility>

namespace scry::detail {

Error make_error(const ErrorCategory category, std::string message,
                 const bool retryable) {
  return Error{
      .category = category,
      .message = std::move(message),
      .retryable = retryable,
  };
}

} // namespace scry::detail
