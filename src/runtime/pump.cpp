#include "runtime/pump.hpp"

#include "core/log.hpp"
#include "runtime/tool_dispatch.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Error registration_error(std::string message) {
  return Error{
      .category = ErrorCategory::invalid_state,
      .message = std::move(message),
  };
}

template <typename Callback>
[[nodiscard]] Status register_once(Callback& destination, Callback callback,
                                   const char* name) {
  if (!callback) {
    return std::unexpected(registration_error(std::string{"cannot register an empty "} +
                                              name + " callback"));
  }
  if (destination) {
    return std::unexpected(
        registration_error(std::string{name} + " callback is already registered"));
  }
  destination = std::move(callback);
  return {};
}

} // namespace

TurnRoute::TurnRoute(const TurnId turn_id, std::shared_ptr<std::atomic<bool>> cancelled,
                     std::weak_ptr<CommandQueue> commands,
                     std::shared_ptr<ConversationState> conversation,
                     std::string user_message, TurnRouteOptions options)
    : turn_id_(turn_id), cancelled_(std::move(cancelled)),
      commands_(std::move(commands)), conversation_(std::move(conversation)),
      user_message_(std::move(user_message)), tools_(std::move(options.tools)),
      max_tool_result_bytes_(options.max_tool_result_bytes),
      remaining_exchange_bytes_(options.max_exchange_bytes),
      max_conversation_bytes_(options.max_conversation_bytes) {}

TurnId TurnRoute::id() const noexcept { return turn_id_; }

std::shared_ptr<std::atomic<bool>> TurnRoute::cancel_flag() const noexcept {
  return cancelled_;
}

bool TurnRoute::cancel() noexcept {
  const auto changed = !cancelled_->exchange(true, std::memory_order_relaxed);
  if (changed) {
    if (const auto commands = commands_.lock()) {
      commands->push(CancelTurnCommand{.turn_id = turn_id_});
    }
  }
  return changed;
}

void TurnRoute::detach() noexcept { attached_ = false; }

bool TurnRoute::attached() const noexcept { return attached_; }

bool TurnRoute::terminal() const noexcept { return terminal_; }

void TurnRoute::mark_terminal() noexcept { terminal_ = true; }

Status TurnRoute::register_text(TextDeltaCallback callback) {
  return register_once(on_text_, std::move(callback), "text-delta");
}

Status TurnRoute::register_tool(ToolCallCallback callback) {
  return register_once(on_tool_, std::move(callback), "tool-call");
}

Status TurnRoute::register_completion(CompletionCallback callback) {
  return register_once(on_completion_, std::move(callback), "completion");
}

Status TurnRoute::register_error(ErrorCallback callback) {
  return register_once(on_error_, std::move(callback), "error");
}

Status TurnRoute::register_cancelled(CancelledCallback callback) {
  return register_once(on_cancelled_, std::move(callback), "cancelled");
}

bool TurnRoute::has_callback(const WorkerEvent& event) const noexcept {
  return std::visit(
      [this](const auto& value) {
        using Event = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Event, TextDeltaEvent>) {
          return static_cast<bool>(on_text_);
        } else if constexpr (std::is_same_v<Event, ToolCallEvent>) {
          return !terminal_ && !tool_dispatch_failed_;
        } else if constexpr (std::is_same_v<Event, CompletionEvent>) {
          return static_cast<bool>(on_completion_);
        } else if constexpr (std::is_same_v<Event, ErrorEvent>) {
          return static_cast<bool>(on_error_);
        } else if constexpr (std::is_same_v<Event, CancelledEvent>) {
          return static_cast<bool>(on_cancelled_);
        }
      },
      event);
}

bool TurnRoute::should_discard(const WorkerEvent& event) const noexcept {
  // Observer events wait for a callback the application may still register. A
  // tool call is instead acted on by the route itself, so one it can no longer
  // dispatch is dead rather than pending.
  return std::holds_alternative<ToolCallEvent>(event) && !has_callback(event);
}

void TurnRoute::invoke(const WorkerEvent& event) {
  std::visit(
      [this](const auto& value) {
        using Event = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Event, TextDeltaEvent>) {
          on_text_(value.text);
        } else if constexpr (std::is_same_v<Event, ToolCallEvent>) {
          dispatch(value);
        } else if constexpr (std::is_same_v<Event, CompletionEvent>) {
          // commit_completion captured the text before moving the exchange
          // into the Conversation.
          const Completion completion{
              .turn_id = value.turn_id,
              .text = value.text,
              .finish_reason = value.finish_reason,
              .usage = value.usage,
              .attempt_count = value.attempt_count,
              .provider_request_id = value.provider_request_id,
          };
          on_completion_(completion);
        } else if constexpr (std::is_same_v<Event, ErrorEvent>) {
          on_error_(value.error);
        } else if constexpr (std::is_same_v<Event, CancelledEvent>) {
          on_cancelled_(Cancelled{.turn_id = value.turn_id});
        }
      },
      event);
}

void TurnRoute::dispatch(const ToolCallEvent& event) {
  if (cancelled_->load(std::memory_order_acquire)) {
    return;
  }
  remaining_exchange_bytes_ =
      std::min(remaining_exchange_bytes_, event.remaining_exchange_bytes);
  SCRY_LOG("Dispatching {} Tool on the app thread (Turn {})", event.call.name,
           turn_id_.value);
  auto result = dispatch_tool(tools_, event.call, max_tool_result_bytes_);
  if (result) {
    const auto result_bytes = content_payload_bytes(*result);
    if (result_bytes > remaining_exchange_bytes_) {
      result = std::unexpected(Error{
          .category = ErrorCategory::resource_limit,
          .message = "tool results exceed the remaining Conversation byte limit",
      });
    } else {
      remaining_exchange_bytes_ -= result_bytes;
    }
  }
  if (!result) {
    tool_dispatch_failed_ = true;
  }
  if (cancelled_->load(std::memory_order_acquire)) {
    return;
  }
  const auto result_ready = result.has_value();
  if (const auto commands = commands_.lock()) {
    commands->push(ToolResultCommand{
        .turn_id = turn_id_,
        .result = std::move(result),
    });
  }
  if (result_ready && on_tool_) {
    notify_tool_observer(event.call);
  }
}

void TurnRoute::notify_tool_observer(const ToolCallBlock& call) {
  on_tool_(ToolCall{
      .turn_id = turn_id_,
      .id = call.id,
      .name = call.name,
      .arguments = call.arguments,
  });
}

const std::shared_ptr<ConversationState>& TurnRoute::conversation() const noexcept {
  return conversation_;
}

const std::string& TurnRoute::user_message() const noexcept { return user_message_; }

std::size_t TurnRoute::max_conversation_bytes() const noexcept {
  return max_conversation_bytes_;
}

} // namespace scry::detail
