#pragma once

#include "core/model.hpp"
#include "runtime/tool_registry_impl.hpp"

#include <cstddef>
#include <scry/error.hpp>
#include <string_view>

namespace scry::detail {

// What the model is told when a handler fails without publishing its own text,
// including when it throws. A throwing admission hook is told the same, so the
// model cannot tell the two apart.
inline constexpr std::string_view handler_failed_message =
    "tool handler returned an error";

// The model-visible error result for a call that produced no usable handler
// value: a refusal, a failed handler, invalid handler JSON, or an unknown tool.
// Falls back to a fixed text and then to a framework failure when the message
// itself will not fit.
[[nodiscard]] Result<ToolResultBlock> error_result(const ToolCallBlock& call,
                                                   std::string_view message,
                                                   std::size_t max_result_bytes);

[[nodiscard]] Result<ToolResultBlock> dispatch_tool(const ToolSnapshot& snapshot,
                                                    const ToolCallBlock& call,
                                                    const ToolCallContext& context,
                                                    std::size_t max_result_bytes);

} // namespace scry::detail
