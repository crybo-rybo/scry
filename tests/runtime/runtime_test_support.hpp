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
#include <scry/events.hpp>
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
registered_tool(std::string name, scry::ToolHandler handler) {
  return std::make_shared<const scry::detail::RegisteredTool>(
      scry::detail::RegisteredTool{
          .definition = tool_definition(std::move(name)),
          .handler = std::make_shared<scry::ToolHandler>(std::move(handler)),
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

// Only the assistant text and the retry bookkeeping ever vary between suites,
// so they travel as defaulted options rather than positional overloads.
struct CompletionOptions {
  std::string text{"done"};
  std::uint32_t attempt_count{1};
  std::string provider_request_id{"request-id"};
};

[[nodiscard]] inline scry::detail::CompletionEvent
completion_event(const scry::TurnId turn_id, CompletionOptions options = {}) {
  return {
      .turn_id = turn_id,
      .exchange = {scry::detail::Message{
          .role = scry::detail::Role::assistant,
          .content = {scry::detail::TextBlock{.text = std::move(options.text)}},
      }},
      .finish_reason = scry::FinishReason::completed,
      .attempt_count = options.attempt_count,
      .provider_request_id = std::move(options.provider_request_id),
  };
}

// Everything a test wants to vary about a route, so one fixture serves the
// tool-dispatch, pump-delivery, and conversation-limit suites without each
// growing its own positional overload.
struct RouteOptions {
  scry::detail::ToolSnapshot tools{};
  std::size_t max_tool_result_bytes{1024};
  std::size_t max_exchange_bytes{std::numeric_limits<std::size_t>::max()};
  std::size_t max_conversation_bytes{1024};
  scry::TurnCallbacks callbacks{};
};

struct PumpFixture {
  std::shared_ptr<scry::detail::CommandQueue> commands{
      std::make_shared<scry::detail::CommandQueue>()};
  std::shared_ptr<scry::detail::EventQueue> events{
      std::make_shared<scry::detail::EventQueue>()};
  std::shared_ptr<scry::detail::ConversationState> conversation{
      std::make_shared<scry::detail::ConversationState>()};

  [[nodiscard]] std::shared_ptr<scry::detail::TurnRoute>
  route(const std::uint64_t id, RouteOptions options = {}) const {
    return std::make_shared<scry::detail::TurnRoute>(
        scry::TurnId{.value = id}, std::make_shared<std::atomic<bool>>(false), commands,
        conversation, "question",
        scry::detail::TurnRouteOptions{
            .tools = std::move(options.tools),
            .max_tool_result_bytes = options.max_tool_result_bytes,
            .max_exchange_bytes = options.max_exchange_bytes,
            .max_conversation_bytes = options.max_conversation_bytes,
            .callbacks = std::move(options.callbacks),
        });
  }
};

} // namespace scry::test_support
