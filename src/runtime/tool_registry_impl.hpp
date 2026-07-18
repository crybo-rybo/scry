#pragma once

#include "runtime/state.hpp"

#include <memory>
#include <scry/tool_registry.hpp>

namespace scry {

class ToolRegistry::Impl final {
public:
  Impl() : state(std::make_shared<detail::ToolRegistryState>()) {}

  std::shared_ptr<detail::ToolRegistryState> state{};
};

} // namespace scry
