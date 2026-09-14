#include "runtime/pump.hpp"

#include "runtime/tool_dispatch.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
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

// The context borrows from the event, which outlives the dispatch that uses it.
[[nodiscard]] ToolCallContext call_context(const TurnId turn_id,
                                           const ToolCallEvent& event) noexcept {
  return {
      .turn_id = turn_id,
      .call_id = event.call.id,
      .tool_name = event.call.name,
      .round = event.round,
      .index = event.index,
  };
}

// Model-visible refusal text for a call the per-turn limit will not admit. It
// tells the model what to do next rather than only what went wrong, because it
// is the model that has to get the turn moving again.
constexpr std::string_view call_limit_message =
    "tool call limit for this turn reached; respond without calling tools";

// An admission hook that throws is indistinguishable, from the model's side,
// from a handler that throws, so it says the same thing.
[[nodiscard]] std::optional<ToolRejection>
consult_admission(ToolAdmissionCallback& hook, const ToolRequest& request) noexcept {
  try {
    return hook(request);
  } catch (...) {
    return ToolRejection{.model_message = "tool handler threw an exception"};
  }
}

// Marks a route as running a callback, and on the way out performs the clear
// that a disconnect made from inside that callback had to defer. It is a guard
// rather than a statement after the call because a host callback may throw, and
// the Harness has to stay valid on that path too.
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
                     TurnRouteOptions options)
    : turn_id_(turn_id), cancelled_(std::move(cancelled)),
      commands_(std::move(commands)), conversation_(std::move(conversation)),
      tools_(std::move(options.tools)),
      max_tool_result_bytes_(options.max_tool_result_bytes),
      remaining_exchange_bytes_(options.max_exchange_bytes),
      max_conversation_bytes_(options.max_conversation_bytes),
      max_tool_calls_(options.max_tool_calls),
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

// Dropping the callbacks is the whole operation: the pump then releases text
// and terminal events instead of delivering them, while tool dispatch and the
// history commit, which belong to the registry and the Conversation, carry on.
// A host may call this from inside a callback the route is running, and the
// callbacks own that closure, so dropping them there would free the running
// frame's own captures; InvocationGuard drops them once the call returns.
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

void TurnRoute::note_pending() noexcept { ++pending_events_; }

void TurnRoute::note_delivered() noexcept {
  if (pending_events_ != 0) {
    --pending_events_;
  }
}

std::size_t TurnRoute::pending_events() const noexcept { return pending_events_; }

// A finished turn never dispatches another tool or reports another outcome, so
// the host closures and the frozen tool snapshot are dead weight from here on.
// Everything the handle still answers for - identity, cancel flag, terminal and
// finished state - is left untouched.
void TurnRoute::retire() noexcept {
  callbacks_ = TurnCallbacks{};
  tools_.reset();
}

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

void TurnRoute::invoke(WorkerEvent& event) {
  const InvocationGuard guard{invoking_, disconnected_, callbacks_};
  std::visit(
      [this](auto& value) {
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
              .tool_round_count = value.tool_round_count,
              .tool_call_count = value.tool_call_count,
              .rejected_tool_call_count = rejected_count_,
              .unexecuted_tool_calls = std::move(value.unexecuted_tool_calls),
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

// Every call that reaches the route counts against the limit, including one
// naming a tool nobody registered: the model spent the turn's budget by asking.
// An unknown tool is refused by dispatch_tool with its own text and the hook is
// not consulted, because there is no handler for the host to admit.
std::optional<Result<ToolResultBlock>> TurnRoute::admit(const ToolCallEvent& event) {
  ++dispatched_count_;
  if (max_tool_calls_ && dispatched_count_ > *max_tool_calls_) {
    ++rejected_count_;
    return error_result(event.call, call_limit_message, max_tool_result_bytes_);
  }
  if (callbacks_.on_tool_request &&
      tool_is_registered(route_tools(tools_), event.call.name)) {
    const auto rejection =
        consult_admission(callbacks_.on_tool_request,
                          ToolRequest{.context = call_context(turn_id_, event),
                                      .arguments = event.call.arguments});
    if (rejection) {
      ++rejected_count_;
      return error_result(event.call, rejection->model_message, max_tool_result_bytes_);
    }
  }
  return std::nullopt;
}

// A hook may cancel the turn and still admit the call. Cancellation is honoured
// before the handler runs, because a handler's side effects on host state are
// the one thing the rollback cannot undo; the limit and the hook have already
// spent their counts by then, and a suppressed call is not a refusal.
std::optional<Result<ToolResultBlock>> TurnRoute::produce(const ToolCallEvent& event) {
  auto refusal = admit(event);
  if (cancelled_->load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  if (refusal) {
    return refusal;
  }
  return dispatch_tool(route_tools(tools_), event.call, call_context(turn_id_, event),
                       max_tool_result_bytes_);
}

void TurnRoute::dispatch(const ToolCallEvent& event) {
  if (cancelled_->load(std::memory_order_acquire)) {
    return;
  }
  remaining_exchange_bytes_ =
      std::min(remaining_exchange_bytes_, event.remaining_exchange_bytes);
  auto produced = produce(event);
  if (!produced) {
    return;
  }
  auto result = std::move(*produced);
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
  // A handler or admission hook that disconnected from inside this dispatch is
  // checked explicitly:
  // InvocationGuard defers clearing the callbacks until the frame returns, so
  // on_tool_call is still set here even though delivery is no longer wanted.
  auto observed = result.has_value() && callbacks_.on_tool_call && !disconnected_
                      ? std::optional<ToolResultBlock>{*result}
                      : std::nullopt;
  if (const auto commands = commands_.lock()) {
    commands->push(ToolResultCommand{
        .turn_id = turn_id_,
        .result = std::move(result),
    });
  }
  if (observed) {
    notify_tool_observer(event, *observed);
  }
}

void TurnRoute::notify_tool_observer(const ToolCallEvent& event,
                                     const ToolResultBlock& result) {
  callbacks_.on_tool_call(ToolCall{
      .turn_id = turn_id_,
      .id = event.call.id,
      .name = event.call.name,
      .arguments = event.call.arguments,
      .result = result.result,
      .is_error = result.is_error,
      .round = event.round,
      .index = event.index,
  });
}

const std::shared_ptr<ConversationState>& TurnRoute::conversation() const noexcept {
  return conversation_;
}

std::size_t TurnRoute::max_conversation_bytes() const noexcept {
  return max_conversation_bytes_;
}

} // namespace scry::detail
