#pragma once

#include "runtime/pump.hpp"
#include "runtime/tool_dispatch.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/tool_registry.hpp>
#include <string>
#include <utility>

namespace scry::test_support {

[[nodiscard]] inline scry::ToolDefinition tool_definition(std::string name) {
  return {
      .name = std::move(name),
      .description = "test tool",
      .input_schema = {.text = "{}"},
  };
}

[[nodiscard]] inline scry::detail::ToolRegistrationPtr
registered_tool(std::string name, scry::ToolHandler handler,
                const scry::ToolExecution execution = scry::ToolExecution::app_thread) {
  auto stored_handler = execution == scry::ToolExecution::app_thread
                            ? std::make_shared<scry::ToolHandler>(std::move(handler))
                            : std::shared_ptr<scry::ToolHandler>{};
  return std::make_shared<const scry::detail::RegisteredTool>(
      scry::detail::RegisteredTool{
          .definition = tool_definition(std::move(name)),
          .execution = execution,
          .handler = std::move(stored_handler),
      });
}

[[nodiscard]] inline scry::detail::ToolCallBlock
tool_call(std::string name = "forecast", std::string id = "call-1") {
  return {
      .id = std::move(id),
      .name = std::move(name),
      .arguments = {.text = R"({"z":2,"a":1})"},
  };
}

[[nodiscard]] inline scry::detail::ToolCallEvent
tool_event(const scry::TurnId turn_id, std::string name = "forecast",
           std::string id = "call-1",
           const std::size_t remaining = std::numeric_limits<std::size_t>::max()) {
  return {
      .turn_id = turn_id,
      .call = tool_call(std::move(name), std::move(id)),
      .remaining_exchange_bytes = remaining,
  };
}

[[nodiscard]] inline scry::detail::CompletionEvent
completion_event(const scry::TurnId turn_id) {
  return {
      .turn_id = turn_id,
      .exchange = {scry::detail::Message{
          .role = scry::detail::Role::assistant,
          .content = {scry::detail::TextBlock{.text = "done"}},
      }},
      .finish_reason = scry::FinishReason::completed,
  };
}

struct PumpFixture {
  std::shared_ptr<scry::detail::CommandQueue> commands{
      std::make_shared<scry::detail::CommandQueue>()};
  std::shared_ptr<scry::detail::EventQueue> events{
      std::make_shared<scry::detail::EventQueue>()};
  std::shared_ptr<scry::detail::ConversationState> conversation{
      std::make_shared<scry::detail::ConversationState>()};

  [[nodiscard]] std::shared_ptr<scry::detail::TurnRoute>
  route(const std::uint64_t id, scry::detail::ToolSnapshot tools,
        const std::size_t result_limit = 1024,
        const std::size_t exchange_limit =
            std::numeric_limits<std::size_t>::max()) const {
    return std::make_shared<scry::detail::TurnRoute>(
        scry::TurnId{.value = id}, std::make_shared<std::atomic<bool>>(false), commands,
        conversation, "question",
        scry::detail::TurnRouteOptions{
            .tools = std::move(tools),
            .max_tool_result_bytes = result_limit,
            .max_exchange_bytes = exchange_limit,
            .max_conversation_bytes = 1024,
        });
  }
};

} // namespace scry::test_support
