#pragma once

#include "core/model.hpp"

#include <cstddef>
#include <scry/conversation.hpp>
#include <scry/json.hpp>
#include <scry/tool_registry.hpp>
#include <string>
#include <vector>

namespace scry::detail {

struct ConversationState {
  ConversationConfig config{};
  std::vector<Message> messages{};
  std::size_t payload_bytes{};
  bool busy{false};
};

struct RegistryEntry {
  ToolDefinition definition{};
  ToolHandler handler{};
};

struct ToolRegistryState {
  std::vector<RegistryEntry> entries{};
};

[[nodiscard]] std::size_t message_payload_bytes(const Message& message) noexcept;
[[nodiscard]] std::string response_text(const ModelResponse& response);

} // namespace scry::detail
