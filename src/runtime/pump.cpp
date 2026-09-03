#include "runtime/pump.hpp"

#include "runtime/tool_dispatch.hpp"

#include <algorithm>
#include <optional>
#include <type_traits>
#include <utility>

namespace scry::detail {
namespace {

template <typename> inline constexpr bool unhandled_worker_event = false;

/// Terminal error reported to on_finished when a turn ends through cancellation.
[[nodiscard]] Error cancellation_error(const TurnId turn_id) {
  return Error{
      .category = ErrorCategory::cancelled,
      .message = "turn cancelled",
      .turn_id = turn_id,
  };
}

[[nodiscard]] const ToolSnapshot& route_tools(const FrozenToolEntries& tools) noexcept {
  static const ToolSnapshot empty{};
  return tools ? *tools : empty;
}

// Marks a route as invoking for the length of one callback and performs the
// clear a disconnect made from inside that callback had to defer. It is RAII
// rather than a statement after the visit because a throwing host callback
// leaves through update() with the Harness still required to be valid, so the
// clear has to happen on the exception path too.
class InvocationGuard final {
public:
  InvocationGuard(bool& invoking, const bool& disconnected,
                  TurnCallbacks& callbacks) noexcept
      : invoking_(invoking), disconnected_(disconnected), callbacks_(callbacks) {
    invoking_ = true;
  }
  InvocationGuard(const InvocationGuard&) = delete;
  InvocationGuard(InvocationGuard&&) = delete;
  InvocationGuard& operator=(const InvocationGuard&) = delete;
  InvocationGuard& operator=(InvocationGuard&&) = delete;
  ~InvocationGuard() {
    invoking_ = false;
    if (disconnected_) {
      callbacks_ = TurnCallbacks{};
    }
  }

private:
  bool& invoking_;
  const bool& disconnected_;
  TurnCallbacks& callbacks_;
};

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
      max_conversation_bytes_(options.max_conversation_bytes),
      callbacks_(std::move(options.callbacks)) {}

TurnId TurnRoute::id() const noexcept { return turn_id_; }

std::shared_ptr<std::atomic<bool>> TurnRoute::cancel_flag() const noexcept {
  return cancelled_;
}

bool TurnRoute::cancel() noexcept {
  if (terminal_) {
    return false;
  }
  const auto changed = !cancelled_->exchange(true, std::memory_order_relaxed);
  if (changed) {
    if (const auto commands = commands_.lock()) {
      commands->push(CancelTurnCommand{.turn_id = turn_id_});
    }
  }
  return changed;
}

// Clearing the callbacks is the whole operation: has_callback then reports
// false for text and terminal events, so the pump releases them instead of
// delivering them, while tool dispatch — which belongs to the registry, not to
// the callbacks — keeps running and history still commits.
//
// A host may disconnect from inside a callback it is currently running, and the
// callbacks own that closure, so clearing them there would free the frame's own
// captures. Such a disconnect only records the intent; InvocationGuard performs
// the clear once the invocation unwinds, which still stops delivery at the very
// next event.
bool TurnRoute::disconnect() noexcept {
  if (disconnected_ || finished()) {
    return false;
  }
  disconnected_ = true;
  if (!invoking_) {
    callbacks_ = TurnCallbacks{};
  }
  return true;
}

void TurnRoute::detach() noexcept { attached_ = false; }

bool TurnRoute::attached() const noexcept { return attached_; }

bool TurnRoute::terminal() const noexcept { return terminal_; }

// A turn is finished once its terminal outcome reached the host, or once it
// would have when no on_finished was supplied.
bool TurnRoute::finished() const noexcept {
  return terminal_ && (!callbacks_.on_finished || terminal_delivered_);
}

void TurnRoute::mark_terminal() noexcept { terminal_ = true; }

bool TurnRoute::has_callback(const WorkerEvent& event) const noexcept {
  return std::visit(
      [this](const auto& value) {
        using Event = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Event, TextDeltaEvent>) {
          return static_cast<bool>(callbacks_.on_text_delta);
        } else if constexpr (std::is_same_v<Event, ToolCallEvent>) {
          // A tool call is acted on by the route itself rather than observed, so
          // one it can no longer dispatch is dead rather than pending.
          return !terminal_ && !tool_dispatch_failed_;
        } else if constexpr (std::is_same_v<Event, CompletionEvent> ||
                             std::is_same_v<Event, ErrorEvent> ||
                             std::is_same_v<Event, CancelledEvent>) {
          return static_cast<bool>(callbacks_.on_finished);
        } else {
          static_assert(unhandled_worker_event<Event>,
                        "TurnRoute::has_callback must classify every WorkerEvent");
        }
      },
      event);
}

void TurnRoute::invoke(const WorkerEvent& event) {
  const InvocationGuard guard{invoking_, disconnected_, callbacks_};
  std::visit(
      [this](const auto& value) {
        using Event = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Event, TextDeltaEvent>) {
          callbacks_.on_text_delta(value.text);
        } else if constexpr (std::is_same_v<Event, ToolCallEvent>) {
          dispatch(value);
        } else if constexpr (std::is_same_v<Event, CompletionEvent>) {
          // commit_completion captured the text before moving the exchange
          // into the Conversation.
          terminal_delivered_ = true;
          callbacks_.on_finished(Completion{
              .turn_id = value.turn_id,
              .text = value.text,
              .finish_reason = value.finish_reason,
              .usage = value.usage,
              .attempt_count = value.attempt_count,
              .provider_request_id = value.provider_request_id,
          });
        } else if constexpr (std::is_same_v<Event, ErrorEvent>) {
          terminal_delivered_ = true;
          callbacks_.on_finished(std::unexpected(value.error));
        } else if constexpr (std::is_same_v<Event, CancelledEvent>) {
          terminal_delivered_ = true;
          callbacks_.on_finished(std::unexpected(cancellation_error(value.turn_id)));
        } else {
          static_assert(unhandled_worker_event<Event>,
                        "TurnRoute::invoke must handle every WorkerEvent");
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
  auto result = dispatch_tool(route_tools(tools_), event.call, max_tool_result_bytes_);
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
  // The observer sees the same result block the model receives, so it is copied
  // out before the command queue takes ownership. A framework failure leaves the
  // result empty and fails the turn instead, and the observer does not fire.
  auto observed = result.has_value() && callbacks_.on_tool_call
                      ? std::optional<ToolResultBlock>{*result}
                      : std::nullopt;
  if (const auto commands = commands_.lock()) {
    commands->push(ToolResultCommand{
        .turn_id = turn_id_,
        .result = std::move(result),
    });
  }
  if (observed) {
    notify_tool_observer(event.call, *observed);
  }
}

void TurnRoute::notify_tool_observer(const ToolCallBlock& call,
                                     const ToolResultBlock& result) {
  callbacks_.on_tool_call(ToolCall{
      .turn_id = turn_id_,
      .id = call.id,
      .name = call.name,
      .arguments = call.arguments,
      .result = result.result,
      .is_error = result.is_error,
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
