#include "runtime/tool_dispatch.hpp"

#include "core/error.hpp"
#include "core/json_codec.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scry::detail {
namespace {

[[nodiscard]] Error oversized_result() {
  return make_error(ErrorCategory::resource_limit,
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

// The model already received every registered name in the request's tool list, so
// naming them back is a reminder rather than a disclosure.
[[nodiscard]] std::string registered_tool_names(const ToolSnapshot& snapshot) {
  std::vector<std::string_view> names;
  names.reserve(snapshot.size());
  for (const auto& registration : snapshot) {
    names.emplace_back(registration->definition.name);
  }
  std::ranges::sort(names);
  std::string joined;
  for (const auto& name : names) {
    if (!joined.empty()) {
      joined.append(", ");
    }
    joined.append(name);
  }
  return joined;
}

[[nodiscard]] std::string unknown_tool_message(const ToolSnapshot& snapshot,
                                               const std::string_view requested) {
  std::string text{"unknown tool \""};
  text.append(requested);
  text.append("\"; ");
  const auto names = registered_tool_names(snapshot);
  if (names.empty()) {
    text.append("no tools are registered");
  } else {
    text.append("registered tools: ");
    text.append(names);
  }
  return text;
}

// What the model is told when a handler fails without publishing its own text.
constexpr std::string_view handler_failed_message = "tool handler returned an error";

[[nodiscard]] Result<ToolResultBlock>
dispatch_tool_handler(ContextualToolHandler& handler, const ToolCallBlock& call,
                      const ToolCallContext& context,
                      const std::size_t max_result_bytes) {
  Result<Json> invoked{};
  try {
    invoked = handler(context, call.arguments);
  } catch (...) {
    return error_result(call, handler_failed_message, max_result_bytes);
  }
  if (!invoked) {
    // Only text the handler deliberately published travels on; `message` and any
    // exception text stay on the host side of the boundary.
    const auto& published = invoked.error().model_message;
    return error_result(
        call, published.empty() ? handler_failed_message : std::string_view{published},
        max_result_bytes);
  }
  return successful_result(call, *invoked, max_result_bytes);
}

} // namespace

Result<ToolResultBlock> error_result(const ToolCallBlock& call,
                                     const std::string_view message,
                                     const std::size_t max_result_bytes) {
  auto payload = make_json_error_object(message);
  if (payload.text.size() > max_result_bytes) {
    payload = make_json_error_object("tool execution failed");
  }
  if (payload.text.size() > max_result_bytes) {
    return std::unexpected(
        make_error(ErrorCategory::resource_limit,
                   "tool error result exceeds the configured byte limit"));
  }
  return ToolResultBlock{
      .tool_call_id = call.id,
      .result = std::move(payload),
      .is_error = true,
  };
}

Result<ToolResultBlock> dispatch_tool(const ToolSnapshot& snapshot,
                                      const ToolCallBlock& call,
                                      const ToolCallContext& context,
                                      const std::size_t max_result_bytes) {
  const auto* const registration = find_tool(snapshot, call.name);
  if (registration == nullptr) {
    return error_result(call, unknown_tool_message(snapshot, call.name),
                        max_result_bytes);
  }
  return dispatch_tool_handler(registration->handler, call, context, max_result_bytes);
}

} // namespace scry::detail
