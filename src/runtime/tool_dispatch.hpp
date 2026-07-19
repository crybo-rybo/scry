#pragma once

#include "core/model.hpp"
#include "runtime/state.hpp"

#include <cstddef>
#include <scry/error.hpp>
#include <string_view>

namespace scry::detail {

[[nodiscard]] ToolRegistrationPtr find_tool_registration(const ToolSnapshot& snapshot,
                                                         std::string_view name);
[[nodiscard]] Result<ToolResultBlock>
dispatch_tool_handler(ToolHandler& handler, const ToolCallBlock& call,
                      std::size_t max_result_bytes);
[[nodiscard]] Result<ToolResultBlock> dispatch_tool(const ToolSnapshot& snapshot,
                                                    const ToolCallBlock& call,
                                                    std::size_t max_result_bytes);

} // namespace scry::detail
