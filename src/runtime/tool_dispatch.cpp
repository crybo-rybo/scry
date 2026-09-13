#include "runtime/tool_dispatch.hpp"

#include "core/json_codec.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Error dispatch_error(const ErrorCategory category, std::string message) {
  return Error{
      .category = category,
      .message = std::move(message),
  };
}

[[nodiscard]] Result<ToolResultBlock> error_result(const ToolCallBlock& call,
                                                   const std::string_view message,
                                                   const std::size_t max_result_bytes) {
  auto payload = make_json_error_object(message);
  if (payload.text.size() > max_result_bytes) {
    payload = make_json_error_object("tool execution failed");
  }
  if (payload.text.size() > max_result_bytes) {
    return std::unexpected(
        dispatch_error(ErrorCategory::resource_limit,
                       "tool error result exceeds the configured byte limit"));
  }
  return ToolResultBlock{
      .tool_call_id = call.id,
      .result = std::move(payload),
      .is_error = true,
  };
}

[[nodiscard]] Result<Json> invoke_handler(ToolHandler& handler,
                                          const ToolCallBlock& call) noexcept {
  try {
    return handler(call.arguments);
  } catch (...) {
    return std::unexpected(
        dispatch_error(ErrorCategory::tool, "tool handler threw an exception"));
  }
}

[[nodiscard]] Error oversized_result() {
  return dispatch_error(ErrorCategory::resource_limit,
                        "tool result exceeds the configured byte limit");
}

[[nodiscard]] Result<ToolResultBlock>
successful_result(const ToolCallBlock& call, const Json& value,
                  const std::size_t max_result_bytes) {
  // Canonicalising parses and re-serialises, so rejecting an already-oversized
  // payload here keeps an unbounded handler return from being parsed at all.
  if (value.text.size() > max_result_bytes) {
    return std::unexpected(oversized_result());
  }
  auto canonical = canonicalize_json(value, ErrorCategory::tool,
                                     "tool handler returned invalid JSON");
  if (!canonical) {
    return error_result(call, canonical.error().message, max_result_bytes);
  }
  if (canonical->text.size() > max_result_bytes) {
    return std::unexpected(oversized_result());
  }
  return ToolResultBlock{
      .tool_call_id = call.id,
      .result = std::move(*canonical),
  };
}

[[nodiscard]] ToolRegistrationPtr find_tool_registration(const ToolSnapshot& snapshot,
                                                         const std::string_view name) {
  const auto found = std::ranges::find_if(snapshot, [name](const auto& registration) {
    return registration->definition.name == name;
  });
  return found == snapshot.end() ? nullptr : *found;
}

[[nodiscard]] Result<ToolResultBlock>
dispatch_tool_handler(ToolHandler& handler, const ToolCallBlock& call,
                      const std::size_t max_result_bytes) {
  auto invoked = invoke_handler(handler, call);
  if (!invoked) {
    return error_result(call, "tool handler returned an error", max_result_bytes);
  }
  return successful_result(call, *invoked, max_result_bytes);
}

} // namespace

Result<ToolResultBlock> dispatch_tool(const ToolSnapshot& snapshot,
                                      const ToolCallBlock& call,
                                      const std::size_t max_result_bytes) {
  const auto registration = find_tool_registration(snapshot, call.name);
  if (!registration) {
    return error_result(call, "model requested an unknown tool", max_result_bytes);
  }
  // Registration rejects an empty handler, so this only catches a snapshot
  // assembled by hand. The call below dereferences the pointer and then invokes
  // the function, so both have to be live.
  if (!registration->handler || !*registration->handler) {
    return error_result(call, "tool handler is unavailable", max_result_bytes);
  }
  return dispatch_tool_handler(*registration->handler, call, max_result_bytes);
}

} // namespace scry::detail
