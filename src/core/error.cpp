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

namespace scry {

Error tool_error(std::string model_message, std::string host_message) {
  // A handler that supplies no host text still wants the refusal in its own logs,
  // and repeating text it already chose to show the model discloses nothing new.
  if (host_message.empty()) {
    host_message = model_message;
  }
  return Error{
      .category = ErrorCategory::tool,
      .message = std::move(host_message),
      .model_message = std::move(model_message),
  };
}

} // namespace scry
