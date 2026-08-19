#pragma once

#include "core/model.hpp"

#include <cstddef>
#include <memory>
#include <scry/conversation.hpp>
#include <scry/json.hpp>
#include <scry/tool_registry.hpp>
#include <vector>

namespace scry::detail {

struct ConversationState {
  ConversationConfig config{};
  // Committed history as a shared immutable-when-shared block. send() hands a
  // const view of this block to the request instead of copying every message;
  // the pump appends at commit, reseating onto a private copy first whenever a
  // live request snapshot still shares the block.
  std::shared_ptr<std::vector<Message>> messages{
      std::make_shared<std::vector<Message>>()};
  std::size_t payload_bytes{};
  bool busy{false};
};

struct RegisteredTool final {
  ToolDefinition definition{};
  std::shared_ptr<ToolHandler> handler{};
};

using ToolRegistrationPtr = std::shared_ptr<const RegisteredTool>;
using ToolSnapshot = std::vector<ToolRegistrationPtr>;
using FrozenToolEntries = std::shared_ptr<const ToolSnapshot>;

// Immutable registry-level snapshot pair. Registration appends to the mutable
// working list only; the frozen views are rebuilt lazily after the next send
// passes admission validation, so rejected sends pay no freeze cost and repeated
// accepted turns share one block per registration generation.
struct ToolSnapshots {
  FrozenToolEntries entries{};
  SchemaSnapshot schemas{};
};

struct ToolRegistryState {
  ToolSnapshot entries{};
  ToolSnapshots frozen{};
};

[[nodiscard]] ToolSnapshots snapshot_tools(ToolRegistryState& state);

} // namespace scry::detail
