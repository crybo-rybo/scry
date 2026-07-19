#pragma once

#include "core/model.hpp"

#include <atomic>
#include <limits>
#include <memory>
#include <scry/error.hpp>
#include <scry/tool_registry.hpp>
#include <scry/turn_id.hpp>
#include <string>
#include <variant>
#include <vector>

namespace scry::detail {

struct RegisterWorkerToolCommand {
  std::string name{};
  ToolHandler handler{};
};

struct SendTurnCommand {
  TurnId turn_id{};
  ModelRequest request{};
  std::vector<std::string> worker_tool_names{};
  std::shared_ptr<std::atomic<bool>> cancelled{};
  std::size_t max_exchange_bytes{std::numeric_limits<std::size_t>::max()};
};

struct CancelTurnCommand {
  TurnId turn_id{};
};

struct ToolResultCommand {
  TurnId turn_id{};
  Result<ToolResultBlock> result{};
};

struct ExecuteWorkerToolCommand {
  TurnId turn_id{};
  ToolCallBlock call{};
};

using WorkerCommand =
    std::variant<RegisterWorkerToolCommand, SendTurnCommand, CancelTurnCommand,
                 ToolResultCommand, ExecuteWorkerToolCommand>;

struct TextDeltaEvent {
  TurnId turn_id{};
  std::string text{};
};

struct ToolCallEvent {
  TurnId turn_id{};
  ToolCallBlock call{};
  std::size_t remaining_exchange_bytes{std::numeric_limits<std::size_t>::max()};
};

struct WorkerToolAcceptedEvent {
  TurnId turn_id{};
  std::string tool_call_id{};
  std::size_t result_payload_bytes{};
};

struct CompletionEvent {
  TurnId turn_id{};
  std::vector<Message> exchange{};
  FinishReason finish_reason{FinishReason::unknown};
  Usage usage{};
  std::uint32_t attempt_count{};
  std::string provider_request_id{};
};

struct ErrorEvent {
  TurnId turn_id{};
  Error error{};
};

struct CancelledEvent {
  TurnId turn_id{};
};

using WorkerEvent = std::variant<TextDeltaEvent, ToolCallEvent, WorkerToolAcceptedEvent,
                                 CompletionEvent, ErrorEvent, CancelledEvent>;

[[nodiscard]] TurnId event_turn_id(const WorkerEvent& event) noexcept;
[[nodiscard]] std::size_t event_payload_bytes(const WorkerEvent& event) noexcept;

} // namespace scry::detail
