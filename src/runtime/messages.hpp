#pragma once

#include "core/model.hpp"

#include <atomic>
#include <memory>
#include <scry/error.hpp>
#include <scry/turn_id.hpp>
#include <string>
#include <variant>

namespace scry::detail {

struct SendTurnCommand {
  TurnId turn_id{};
  ModelRequest request{};
  std::shared_ptr<std::atomic<bool>> cancelled{};
};

struct CancelTurnCommand {
  TurnId turn_id{};
};

using WorkerCommand = std::variant<SendTurnCommand, CancelTurnCommand>;

struct TextDeltaEvent {
  TurnId turn_id{};
  std::string text{};
};

struct CompletionEvent {
  TurnId turn_id{};
  ModelResponse response{};
  std::uint32_t attempt_count{};
};

struct ErrorEvent {
  TurnId turn_id{};
  Error error{};
};

struct CancelledEvent {
  TurnId turn_id{};
};

using WorkerEvent =
    std::variant<TextDeltaEvent, CompletionEvent, ErrorEvent, CancelledEvent>;

[[nodiscard]] TurnId event_turn_id(const WorkerEvent& event) noexcept;
[[nodiscard]] std::size_t event_payload_bytes(const WorkerEvent& event) noexcept;

} // namespace scry::detail
