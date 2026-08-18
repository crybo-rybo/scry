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
  SharedHistory messages{};
  std::size_t payload_bytes{};
  bool busy{false};
};

struct RegisteredTool final {
  ToolDefinition definition{};
  std::shared_ptr<ToolHandler> handler{};
};

using ToolRegistrationPtr = std::shared_ptr<const RegisteredTool>;
using ToolSnapshot = std::vector<ToolRegistrationPtr>;

struct ToolRegistryState {
  ToolSnapshot entries{};
  std::shared_ptr<const std::vector<ToolSchema>> schema_snapshot =
      std::make_shared<const std::vector<ToolSchema>>();
};

[[nodiscard]] ToolSnapshot snapshot_tools(const ToolRegistryState& state);
[[nodiscard]] std::shared_ptr<const std::vector<ToolSchema>>
schema_snapshot(const ToolRegistryState& state);
[[nodiscard]] std::vector<ToolSchema> snapshot_schemas(const ToolSnapshot& snapshot);

} // namespace scry::detail
