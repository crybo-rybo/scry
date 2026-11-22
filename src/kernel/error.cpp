#include "kernel/kernel.hpp"

#include "kernel/error.hpp"

#include <scrydantic/error.hpp>
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
  return tool_error(
      scrydantic::rejection(std::move(model_message), std::move(host_message)));
}

Error tool_error(scrydantic::Error error) {
  return Error{
      .category = ErrorCategory::tool,
      .message = std::move(error.message),
      .model_message = std::move(error.model_message),
      .issues = std::move(error.issues),
  };
}

} // namespace scry
