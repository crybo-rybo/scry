#pragma once

#include "core/model.hpp"
#include "runtime/state.hpp"

#include <cstddef>
#include <scry/error.hpp>

namespace scry::detail {

[[nodiscard]] Result<ToolResultBlock> dispatch_tool(const ToolSnapshot& snapshot,
                                                    const ToolCallBlock& call,
                                                    const ToolCallContext& context,
                                                    std::size_t max_result_bytes);

} // namespace scry::detail
