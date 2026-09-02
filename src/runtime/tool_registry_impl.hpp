/// @file
/// @brief Internal additive ToolRegistry storage and snapshot operations.
///
/// Registrations remain mutable only in the pump-owned working set. Accepted turns
/// share a lazily frozen registration/schema pair so reentrant registration affects
/// later turns without invalidating an in-flight snapshot.

#pragma once

#include "runtime/state.hpp"

#include <scry/tool_registry.hpp>

namespace scry::detail {

/// Validates and appends one explicit-schema registration.
///
/// The schema is parsed as an object and canonicalized before insertion. The operation
/// is transactional with respect to the registry: invalid definitions, empty handlers,
/// and duplicate names leave @p state unchanged.
///
/// @param state Pump-owned registry state to extend.
/// @param definition Provider-visible definition transferred on success.
/// @param handler Move-only application callable transferred on success.
/// @return Success, or ErrorCategory::invalid_state for an invalid registration.
[[nodiscard]] Status add_tool_registration(ToolRegistryState& state,
                                           ToolDefinition definition,
                                           ToolHandler handler);

} // namespace scry::detail

namespace scry {

/// Private implementation of the Harness-owned ToolRegistry handle.
///
/// The implementation is used only on the app/pump side. Worker requests receive
/// neutral schemas from snapshot(), while application handlers never cross the thread
/// boundary.
class ToolRegistry::Impl final {
public:
  /// Validates and appends an explicit-schema registration.
  ///
  /// @param definition Provider-visible tool definition.
  /// @param handler App-thread callable to own.
  /// @return Success or the immediate registration error.
  [[nodiscard]] Status add(ToolDefinition definition, ToolHandler handler);

  /// Returns the current immutable registration and schema snapshots.
  ///
  /// A changed working set is frozen lazily on this call; an unchanged generation
  /// reuses the existing shared blocks.
  ///
  /// @return Matched handler-entry and neutral-schema snapshots.
  [[nodiscard]] detail::ToolSnapshots snapshot() {
    return detail::snapshot_tools(state);
  }

  /// Pump-owned mutable registrations and their cached frozen generation.
  detail::ToolRegistryState state{};
};

} // namespace scry
