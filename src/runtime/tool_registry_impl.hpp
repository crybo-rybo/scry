#pragma once

#include "runtime/queue.hpp"
#include "runtime/state.hpp"

#include <memory>
#include <scry/tool_registry.hpp>
#include <utility>

namespace scry::detail {

[[nodiscard]] Status add_tool_registration(ToolRegistryState& state,
                                           CommandQueue& commands,
                                           ToolDefinition definition,
                                           ToolHandler handler,
                                           ToolRegistrationOptions options = {});

} // namespace scry::detail

namespace scry {

class ToolRegistry::Impl final {
public:
  void bind_command_queue(std::weak_ptr<detail::CommandQueue> commands) noexcept {
    commands_ = std::move(commands);
  }

  [[nodiscard]] Status add(ToolDefinition definition, ToolHandler handler,
                           ToolRegistrationOptions options);

  [[nodiscard]] detail::ToolSnapshot snapshot() const {
    return detail::snapshot_tools(state);
  }

  detail::ToolRegistryState state{};

private:
  std::weak_ptr<detail::CommandQueue> commands_{};
};

} // namespace scry
