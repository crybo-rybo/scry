#pragma once

#include "core/model.hpp"

#include <memory>
#include <scry/tool_registry.hpp>
#include <string_view>
#include <vector>

namespace scry::detail {

struct RegisteredTool final {
  ToolDefinition definition{};
  // Snapshots share registrations as const, and calling a UniqueFunction is
  // non-const. Handlers only ever run on the host thread, inside update().
  mutable ContextualToolHandler handler{};
};

// Registrations store one handler shape so dispatch has one call path. A plain
// handler is adapted here rather than at every call site; an empty one stays
// empty so registration still rejects it.
[[nodiscard]] ContextualToolHandler to_contextual_handler(ToolHandler handler);

using ToolRegistrationPtr = std::shared_ptr<const RegisteredTool>;
using ToolSnapshot = std::vector<ToolRegistrationPtr>;
using FrozenToolEntries = std::shared_ptr<const ToolSnapshot>;

// The registrations and the schemas derived from them, frozen together so a
// turn can share both without copying. Rebuilt lazily on the first send after a
// registration, so a rejected send never pays for the rebuild.
struct FrozenToolSnapshot {
  FrozenToolEntries entries{};
  SchemaSnapshot schemas{};
};

/// The registration named `name`, or nullptr.
[[nodiscard]] const RegisteredTool* find_tool(const ToolSnapshot& tools,
                                              std::string_view name) noexcept;

} // namespace scry::detail

namespace scry {

class ToolRegistry::Impl final {
public:
  [[nodiscard]] Status add(ToolDefinition definition, ContextualToolHandler handler);
  [[nodiscard]] detail::FrozenToolSnapshot snapshot();
  [[nodiscard]] const detail::ToolSnapshot& entries() const noexcept {
    return entries_;
  }

private:
  detail::ToolSnapshot entries_{};
  detail::FrozenToolSnapshot frozen_{};
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
