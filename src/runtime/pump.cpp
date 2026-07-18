#include "runtime/pump.hpp"

#include <algorithm>
#include <cassert>
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

class UpdateGuard final {
public:
  explicit UpdateGuard(bool& updating) noexcept : updating_(updating) {
    updating_ = true;
  }
  ~UpdateGuard() { updating_ = false; }

  UpdateGuard(const UpdateGuard&) = delete;
  UpdateGuard& operator=(const UpdateGuard&) = delete;

private:
  bool& updating_;
};

[[nodiscard]] std::chrono::steady_clock::time_point
update_deadline(const std::chrono::steady_clock::time_point started,
                const std::optional<std::chrono::microseconds> time_budget) {
  if (!time_budget) {
    return std::chrono::steady_clock::time_point::max();
  }
  if (*time_budget <= std::chrono::microseconds{0}) {
    return started;
  }
  const auto capacity = std::chrono::steady_clock::time_point::max() - started;
  return started +
         std::min(*time_budget,
                  std::chrono::duration_cast<std::chrono::microseconds>(capacity));
}

} // namespace

TurnRoute::TurnRoute(const TurnId turn_id, std::shared_ptr<std::atomic<bool>> cancelled,
                     std::weak_ptr<CommandQueue> commands,
                     std::shared_ptr<ConversationState> conversation,
                     std::string user_message, const std::size_t max_conversation_bytes)
    : turn_id_(turn_id), cancelled_(std::move(cancelled)),
      commands_(std::move(commands)), conversation_(std::move(conversation)),
      user_message_(std::move(user_message)),
      max_conversation_bytes_(max_conversation_bytes) {}

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
        } else if constexpr (std::is_same_v<Event, CompletionEvent>) {
          return static_cast<bool>(on_completion_);
        } else if constexpr (std::is_same_v<Event, ErrorEvent>) {
          return static_cast<bool>(on_error_);
        } else {
          return static_cast<bool>(on_cancelled_);
        }
      },
      event);
}

void TurnRoute::invoke(const WorkerEvent& event) {
  std::visit(
      [this](const auto& value) {
        using Event = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Event, TextDeltaEvent>) {
          on_text_(value.text);
        } else if constexpr (std::is_same_v<Event, CompletionEvent>) {
          const Completion completion{
              .turn_id = value.turn_id,
              .text = response_text(value.response),
              .finish_reason = value.response.finish_reason,
              .usage = value.response.usage,
              .attempt_count = value.attempt_count,
              .provider_request_id = value.response.provider_request_id,
          };
          on_completion_(completion);
        } else if constexpr (std::is_same_v<Event, ErrorEvent>) {
          on_error_(value.error);
        } else {
          on_cancelled_(Cancelled{.turn_id = value.turn_id});
        }
      },
      event);
}

const std::shared_ptr<ConversationState>& TurnRoute::conversation() const noexcept {
  return conversation_;
}

const std::string& TurnRoute::user_message() const noexcept { return user_message_; }

std::size_t TurnRoute::max_conversation_bytes() const noexcept {
  return max_conversation_bytes_;
}

PumpState::PumpState(std::shared_ptr<EventQueue> events, PumpClock clock)
    : events_(std::move(events)), clock_(std::move(clock)) {
  if (!clock_) {
    clock_ = [] { return std::chrono::steady_clock::now(); };
  }
}

void PumpState::add_route(std::shared_ptr<TurnRoute> route) {
  routes_.emplace(route->id(), std::move(route));
}

std::shared_ptr<TurnRoute> PumpState::find_route(const TurnId turn_id) const {
  const auto found = routes_.find(turn_id);
  return found == routes_.end() ? nullptr : found->second;
}

std::size_t PumpState::route_count() const noexcept { return routes_.size(); }

std::size_t PumpState::live_route_count() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      routes_, [](const auto& entry) { return !entry.second->terminal(); }));
}

bool PumpState::updating() const noexcept { return updating_; }

UpdateStats PumpState::update(const UpdateOptions options) {
  if (updating_) {
    return UpdateStats{
        .events_remaining = pending_callbacks_.size() + events_->size(),
        .budget_exhausted = true,
        .reentrant_update_rejected = true,
    };
  }
  UpdateGuard guard{updating_};
  const auto started = clock_();
  const auto deadline = update_deadline(started, options.time_budget);
  std::size_t delivered = 0;
  bool exhausted = ingest_events(deadline);
  while (delivered < options.max_callbacks && has_deliverable()) {
    if (clock_() >= deadline) {
      exhausted = true;
      break;
    }
    if (!deliver_one(delivered)) {
      break;
    }
  }
  if (!exhausted) {
    release_discarded();
    clean_routes();
  }
  const auto remaining = pending_callbacks_.size() + events_->size();
  exhausted = exhausted || (delivered == options.max_callbacks && has_deliverable());
  return UpdateStats{
      .callbacks_delivered = delivered,
      .events_remaining = remaining,
      .budget_exhausted = exhausted,
  };
}

void PumpState::shutdown() noexcept {
  for (auto& [turn_id, route] : routes_) {
    static_cast<void>(turn_id);
    route->conversation()->busy = false;
  }
  for (const auto& event : pending_callbacks_) {
    events_->release(event_turn_id(event.event), event.accounted_bytes);
  }
  pending_callbacks_.clear();
  while (auto event = events_->try_pop()) {
    events_->release(*event);
  }
  routes_.clear();
}

bool PumpState::ingest_events(const std::chrono::steady_clock::time_point deadline) {
  while (events_->size() != 0) {
    if (clock_() >= deadline) {
      return true;
    }
    auto event = events_->try_pop();
    if (!event) {
      return false;
    }
    accept_event(std::move(*event));
  }
  return false;
}

void PumpState::accept_event(WorkerEvent event) {
  const auto accounted_bytes = event_payload_bytes(event);
  const auto route = find_route(event_turn_id(event));
  if (!route) {
    events_->release(event);
    return;
  }
  if (route->terminal()) {
    events_->release(event);
    return;
  }

  apply_terminal(*route, event);
  if (route->attached() || route->has_callback(event)) {
    if (const auto* delta = std::get_if<TextDeltaEvent>(&event);
        delta != nullptr && coalesce_pending_delta(*delta, accounted_bytes)) {
      return;
    }
    pending_callbacks_.push_back(PendingCallback{
        .event = std::move(event),
        .accounted_bytes = accounted_bytes,
    });
  } else {
    events_->release(event);
  }
}

bool PumpState::coalesce_pending_delta(const TextDeltaEvent& event,
                                       const std::size_t accounted_bytes) {
  const auto found =
      std::ranges::find_if(pending_callbacks_, [&event](const auto& pending) {
        const auto* delta = std::get_if<TextDeltaEvent>(&pending.event);
        return delta != nullptr && delta->turn_id == event.turn_id;
      });
  if (found == pending_callbacks_.end()) {
    return false;
  }
  std::get<TextDeltaEvent>(found->event).text += event.text;
  found->accounted_bytes += accounted_bytes;
  return true;
}

void PumpState::apply_terminal(TurnRoute& route, WorkerEvent& event) {
  if (const auto* completion = std::get_if<CompletionEvent>(&event)) {
    if (conversation_limit_exceeded(route, *completion)) {
      event = ErrorEvent{
          .turn_id = completion->turn_id,
          .error =
              Error{
                  .category = ErrorCategory::resource_limit,
                  .message = "completion exceeds the Conversation byte limit",
                  .turn_id = completion->turn_id,
                  .attempt = completion->attempt_count,
                  .provider_request_id = completion->response.provider_request_id,
              },
      };
    } else {
      commit_completion(route, *completion);
    }
  }
  if (std::holds_alternative<CompletionEvent>(event) ||
      std::holds_alternative<ErrorEvent>(event) ||
      std::holds_alternative<CancelledEvent>(event)) {
    route.conversation()->busy = false;
    route.mark_terminal();
  }
}

bool PumpState::conversation_limit_exceeded(
    const TurnRoute& route, const CompletionEvent& event) const noexcept {
  const auto user_bytes = route.user_message().size();
  std::size_t assistant_bytes = 0;
  for (const auto& block : event.response.content) {
    assistant_bytes +=
        message_payload_bytes(Message{.role = Role::assistant, .content = {block}});
  }
  const auto current = route.conversation()->payload_bytes;
  const auto limit = route.max_conversation_bytes();
  return current > limit || user_bytes > limit - current ||
         assistant_bytes > limit - current - user_bytes;
}

void PumpState::commit_completion(TurnRoute& route, const CompletionEvent& event) {
  auto& conversation = *route.conversation();
  Message user{
      .role = Role::user,
      .content = {TextBlock{.text = route.user_message()}},
  };
  Message assistant{
      .role = Role::assistant,
      .content = event.response.content,
  };
  conversation.payload_bytes +=
      message_payload_bytes(user) + message_payload_bytes(assistant);
  conversation.messages.push_back(std::move(user));
  conversation.messages.push_back(std::move(assistant));
}

bool PumpState::deliver_one(std::size_t& callbacks_delivered) {
  const auto found = std::find_if(
      pending_callbacks_.begin(), pending_callbacks_.end(), [this](const auto& event) {
        const auto route = find_route(event_turn_id(event.event));
        return route && route->has_callback(event.event);
      });
  if (found == pending_callbacks_.end()) {
    return false;
  }
  auto pending = std::move(*found);
  pending_callbacks_.erase(found);
  const auto route = find_route(event_turn_id(pending.event));
  events_->release(event_turn_id(pending.event), pending.accounted_bytes);
  ++callbacks_delivered;
  route->invoke(pending.event);
  return true;
}

bool PumpState::has_deliverable() const noexcept {
  return std::ranges::any_of(pending_callbacks_, [this](const auto& event) {
    const auto route = find_route(event_turn_id(event.event));
    return route && route->has_callback(event.event);
  });
}

void PumpState::release_discarded() {
  std::erase_if(pending_callbacks_, [this](const auto& event) {
    const auto route = find_route(event_turn_id(event.event));
    const auto discard =
        !route || (!route->attached() && !route->has_callback(event.event));
    if (discard) {
      events_->release(event_turn_id(event.event), event.accounted_bytes);
    }
    return discard;
  });
}

void PumpState::clean_routes() {
  std::erase_if(routes_, [this](const auto& entry) {
    const auto& [turn_id, route] = entry;
    if (!route->terminal() || route->attached()) {
      return false;
    }
    return std::ranges::none_of(pending_callbacks_, [turn_id](const auto& event) {
      return event_turn_id(event.event) == turn_id;
    });
  });
}

} // namespace scry::detail
