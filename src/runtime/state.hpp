/// @file
/// @brief Pump-owned Conversation and ToolRegistry state shared by runtime facades.
///
/// This file collects state that must survive moves of public PImpl handles while
/// preserving the actor boundary. Conversation routes may share pump-side lifetime, and
/// accepted worker requests may share immutable history/schema blocks; mutable handlers
/// and registry state remain on the app thread.

#pragma once

#include "core/model.hpp"

#include <cstddef>
#include <memory>
#include <scry/conversation.hpp>
#include <scry/json.hpp>
#include <scry/tool_registry.hpp>
#include <vector>

namespace scry::detail {

/// Mutable state behind one Conversation handle.
///
/// The app thread and pump own this object. An accepted model request shares only the
/// `const` view of @ref messages carried by ModelRequest. Before terminal success
/// mutates history, the pump copy-on-write reseats the block if another request still
/// observes it.
struct ConversationState {
  /// Conversation-level configuration, including the persistent system prompt.
  ConversationConfig config{};
  /// Committed provider-neutral history, immutable whenever the block is shared.
  ///
  /// Harness::send() gives the worker request a const view instead of copying every
  /// message. The pump appends only at successful terminal delivery and first creates a
  /// private copy whenever a live request snapshot still shares the block.
  std::shared_ptr<std::vector<Message>> messages{
      std::make_shared<std::vector<Message>>()};
  /// Payload bytes in the system prompt and every committed message.
  ///
  /// This is allocation-independent accounting for the configured Conversation limit.
  std::size_t payload_bytes{};
  /// Whether this Conversation already has one accepted, nonterminal turn.
  ///
  /// Set during acceptance before worker command publication, then cleared by terminal
  /// pump processing or Harness shutdown; it never crosses to the worker.
  bool busy{false};
};

/// One validated explicit-schema tool registration.
///
/// Registration records are immutable after publication into a snapshot. The shared
/// handler wrapper permits generations to reuse a move-only ToolHandler while keeping
/// the callable exclusively on the pump side.
struct RegisteredTool final {
  /// Canonical provider-visible name, description, and object schema.
  ToolDefinition definition{};
  /// Move-only app-thread handler behind stable shared snapshot lifetime.
  std::shared_ptr<ToolHandler> handler{};
};

/// Immutable shared pointer to one validated registration record.
using ToolRegistrationPtr = std::shared_ptr<const RegisteredTool>;
/// Ordered registration collection used for pump-side name lookup and dispatch.
using ToolSnapshot = std::vector<ToolRegistrationPtr>;
/// Immutable accepted-turn view of the ordered registration collection.
using FrozenToolEntries = std::shared_ptr<const ToolSnapshot>;

/// Matched immutable handler and schema views for one registry generation.
///
/// Registration appends only to the mutable working list. The frozen pair is rebuilt
/// lazily after the next send passes admission validation, so rejected sends pay no
/// freeze cost and repeated accepted turns share one block per generation.
struct ToolSnapshots {
  /// Pump-side registration records, including application handlers.
  FrozenToolEntries entries{};
  /// Worker-safe neutral schemas corresponding positionally to @ref entries.
  SchemaSnapshot schemas{};
};

/// Pump-owned ToolRegistry working set and lazy immutable snapshot cache.
struct ToolRegistryState {
  /// Additive validated registrations in provider publication order.
  ToolSnapshot entries{};
  /// Last frozen generation; empty or stale after a successful registration.
  ToolSnapshots frozen{};
};

/// Freezes or reuses the immutable views for the current registry generation.
///
/// The returned pair always describes the same ordered registrations. Only its neutral
/// schema block is safe to pass to the worker; handler entries remain pump-owned.
///
/// @param state Mutable registry whose cached generation may be refreshed.
/// @return Shared registration and schema snapshots for the current working set.
[[nodiscard]] ToolSnapshots snapshot_tools(ToolRegistryState& state);

} // namespace scry::detail
