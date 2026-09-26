#pragma once

#include "core/model.hpp"
#include "machine/turn_machine.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <scry/error.hpp>
#include <scry/turn_id.hpp>
#include <string>
#include <variant>
#include <vector>

namespace scry::detail {

struct SendTurnCommand {
  TurnId turn_id{};
  ModelRequest request{};
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

using WorkerCommand =
    std::variant<SendTurnCommand, CancelTurnCommand, ToolResultCommand>;

struct TextDeltaEvent {
  TurnId turn_id{};
  std::string text{};
};

// The machine's publication already carries exactly what the pump needs.
using ToolCallEvent = PublishToolCall;

// The pump moves `transcript` into the Conversation and keeps `text`, a copy of
// the final assistant text, for the completion callback. The transcript opens
// with the turn's user message and was reserved against the Conversation budget
// by the machine, so the queue charges neither it nor `text`. The calls
// dropped at the tool-round limit were reserved the same way and are charged the
// same nothing.
struct CompletionEvent {
  TurnId turn_id{};
  std::vector<Message> transcript{};
  std::string text{};
  FinishReason finish_reason{FinishReason::unknown};
  Usage usage{};
  std::uint32_t attempt_count{};
  std::string provider_request_id{};
  std::uint32_t tool_round_count{};
  std::uint32_t tool_call_count{};
  std::vector<ToolCallBlock> unexecuted_tool_calls{};
};

struct ErrorEvent {
  TurnId turn_id{};
  Error error{};
};

using CancelledEvent = PublishCancelled;

using WorkerEvent = std::variant<TextDeltaEvent, ToolCallEvent, CompletionEvent,
                                 ErrorEvent, CancelledEvent>;

[[nodiscard]] inline TurnId event_turn_id(const WorkerEvent& event) noexcept {
  return std::visit([](const auto& value) { return value.turn_id; }, event);
}

} // namespace scry::detail
