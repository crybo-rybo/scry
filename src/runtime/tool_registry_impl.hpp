#pragma once

#include "runtime/state.hpp"

#include <scry/tool_registry.hpp>

namespace scry::detail {

[[nodiscard]] Status add_tool_registration(ToolRegistryState& state,
                                           ToolDefinition definition,
                                           ToolHandler handler);

} // namespace scry::detail

namespace scry {

class ToolRegistry::Impl final {
public:
  [[nodiscard]] Status add(ToolDefinition definition, ToolHandler handler);

  [[nodiscard]] detail::ToolSnapshot snapshot() const {
    return detail::snapshot_tools(state);
  }

  detail::ToolRegistryState state{};
};

} // namespace scry
