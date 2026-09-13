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
  // Committed history, held in one shared block. send() hands the request a
  // const view of it instead of copying every message, so the pump copies the
  // block before appending whenever an in-flight request still shares it.
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

// The registrations and the schemas derived from them, frozen together so a
// turn can share both without copying. Rebuilt lazily on the first send after a
// registration, so a rejected send never pays for the rebuild.
struct FrozenToolSnapshot {
  FrozenToolEntries entries{};
  SchemaSnapshot schemas{};
};

struct ToolRegistryState {
  ToolSnapshot entries{};
  FrozenToolSnapshot frozen{};
};

[[nodiscard]] FrozenToolSnapshot snapshot_tools(ToolRegistryState& state);

} // namespace scry::detail
