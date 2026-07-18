#pragma once

#include "runtime/state.hpp"

#include <memory>
#include <scry/tool_registry.hpp>

namespace scry {

class ToolRegistry::Impl final {
public:
  [[nodiscard]] detail::ToolSnapshot snapshot() const {
    return detail::snapshot_tools(state);
  }

  detail::ToolRegistryState state{};
};

} // namespace scry
