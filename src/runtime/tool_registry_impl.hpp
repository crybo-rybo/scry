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

  [[nodiscard]] detail::FrozenToolSnapshot snapshot() {
    return detail::snapshot_tools(state);
  }

  detail::ToolRegistryState state{};
};

} // namespace scry

namespace scry::detail {

/// Reaches the registry's implementation from the runtime without exposing it
/// on the public surface. Only a Harness needs this; hosts use ToolRegistry.
class ToolRegistryAccess final {
public:
  /// Restores an inactive (moved-from) registry to an empty active one, so a
  /// Harness handed such a registry still offers a usable tools().
  static void ensure_active(ToolRegistry& tools);

  /// Freezes the registrations and derived schemas for one accepted turn.
  /// @param tools Active registry; ensure_active() runs first.
  /// @return The shared snapshot.
  [[nodiscard]] static FrozenToolSnapshot snapshot(ToolRegistry& tools);
};

} // namespace scry::detail
