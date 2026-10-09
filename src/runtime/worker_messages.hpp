#pragma once

#include "core/model.hpp"
#include "machine/turn_machine.hpp"

#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/turn_id.hpp>
#include <string>
#include <variant>

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

// The host accepted a typed turn's answer. A rejected one is posted as an
// ordinary ToolResultCommand carrying the error the model is given.
struct AnswerAcceptedCommand {
  TurnId turn_id{};
  std::string call_id{};
};

using WorkerCommand = std::variant<SendTurnCommand, CancelTurnCommand,
                                   ToolResultCommand, AnswerAcceptedCommand>;

struct TextDeltaEvent {
  TurnId turn_id{};
  std::string text{};
};

// The machine's publication already carries exactly what the pump needs.
using ToolCallEvent = PublishToolCall;

// The machine's completion, plus the two values the pump derives from it. The
// pump moves `transcript` into the Conversation and fills `text` with the final
// assistant text for the completion callback. The transcript opens with the
// turn's user message. It and the calls dropped at the tool-round limit are
// already reserved against the Conversation budget - the user message by send(),
// the rest by the machine - so the queue charges neither them nor `text`. An
// answered turn's transcript ends with the answer's text block; the pump copies
// it into `structured` and leaves it out of `text`, uncharged for the same reason.
struct CompletionEvent : CommitCompletion {
  std::string text{};
  std::optional<Json> structured{};
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
