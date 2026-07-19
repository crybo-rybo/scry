#pragma once

#include "core/model.hpp"

#include <cstddef>
#include <memory>
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

struct RegisteredTool final {
  ToolDefinition definition{};
  ToolExecution execution{ToolExecution::app_thread};
  std::shared_ptr<ToolHandler> handler{};
};

using ToolRegistrationPtr = std::shared_ptr<const RegisteredTool>;
using ToolSnapshot = std::vector<ToolRegistrationPtr>;

struct ToolRegistryState {
  ToolSnapshot entries{};
};

[[nodiscard]] ToolSnapshot snapshot_tools(const ToolRegistryState& state);
[[nodiscard]] std::vector<ToolSchema> snapshot_schemas(const ToolSnapshot& snapshot);
[[nodiscard]] std::vector<std::string>
snapshot_worker_tool_names(const ToolSnapshot& snapshot);

[[nodiscard]] std::string response_text(const ModelResponse& response);

} // namespace scry::detail
