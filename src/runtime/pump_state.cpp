#include "runtime/pump.hpp"

#include <algorithm>

namespace scry::detail {
namespace {

[[nodiscard]] std::string completion_text(const CompletionEvent& event) {
  if (event.exchange.empty()) {
    return {};
  }
  const auto& final_message = event.exchange.back();
  std::string text;
  for (const auto& block : final_message.content) {
    if (const auto* value = std::get_if<TextBlock>(&block)) {
      text += value->text;
    }
  }
  return text;
}

// Appends one committed exchange onto the Conversation's history block. The
// block is shared with accepted-turn request snapshots, so a commit reseats it
// copy-on-write whenever a live request still holds it. Terminalization releases
// the worker's request before publishing completion, so ordinary commit reuses
// the block deterministically; retained external snapshots still take this path.
void append_history(ConversationState& conversation, const std::string_view user_text,
                    std::vector<Message>&& exchange) {
  if (conversation.messages.use_count() > 1) {
    conversation.messages =
        std::make_shared<std::vector<Message>>(*conversation.messages);
  }
  auto& messages = *conversation.messages;
  Message user{
      .role = Role::user,
      .content = {TextBlock{.text = std::string{user_text}}},
  };
  conversation.payload_bytes += message_payload_bytes(user);
  messages.push_back(std::move(user));
  for (auto& message : exchange) {
    conversation.payload_bytes += message_payload_bytes(message);
    messages.push_back(std::move(message));
  }
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
    };
  }
  UpdateGuard guard{updating_};
  const auto started = clock_();
  const auto deadline = update_deadline(started, options.time_budget);
  std::size_t delivered = 0;
  bool exhausted = ingest_events(deadline);
  while (delivered < options.max_callbacks && has_deliverable()) {
    // The first deliverable callback is delivered without consulting the
    // deadline, for the same reason the first event is ingested without one.
    if (delivered > 0 && clock_() >= deadline) {
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
  // Every call ingests at least one queued event: the deadline is consulted
  // only before the second and later pops, so a small or already-expired
  // budget can never starve the pump.
  bool ingested = false;
  while (events_->size() != 0) {
    if (ingested && clock_() >= deadline) {
      return true;
    }
    auto event = events_->try_pop();
    if (!event) {
      return false;
    }
    ingested = true;
    accept_event(std::move(*event));
  }
  return false;
}

void PumpState::accept_event(WorkerEvent event) {
  // Every release below credits the size measured here on arrival. Remeasuring
  // later would under-credit a committed completion, whose exchange has by then
  // moved into the Conversation, and strand the remainder in the queue's
  // per-turn byte ledger.
  const auto turn_id = event_turn_id(event);
  const auto accounted_bytes = event_payload_bytes(event);
  const auto route = find_route(turn_id);
  if (!route || route->terminal()) {
    events_->release(turn_id, accounted_bytes);
    return;
  }

  // Terminal handling runs before the retention test so that a turn whose
  // callbacks are empty still commits its history and clears the conversation.
  apply_terminal(*route, event);
  // Callbacks are attached when the turn is accepted, so an event no route
  // callback can consume is dead on arrival rather than awaiting a later
  // registration. Releasing it here returns its bytes to the per-turn ledger
  // immediately.
  if (!route->has_callback(event)) {
    events_->release(turn_id, accounted_bytes);
    return;
  }
  if (const auto* delta = std::get_if<TextDeltaEvent>(&event);
      delta != nullptr && coalesce_pending_delta(*delta, accounted_bytes)) {
    return;
  }
  pending_callbacks_.push_back(PendingCallback{
      .event = std::move(event),
      .accounted_bytes = accounted_bytes,
  });
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
  if (auto* completion = std::get_if<CompletionEvent>(&event)) {
    if (conversation_limit_exceeded(route, *completion)) {
      event = ErrorEvent{
          .turn_id = completion->turn_id,
          .error =
              Error{
                  .category = ErrorCategory::resource_limit,
                  .attempt = completion->attempt_count,
                  .message = "completion exceeds the Conversation byte limit",
                  .turn_id = completion->turn_id,
                  .provider_request_id = completion->provider_request_id,
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
  const auto current = route.conversation()->payload_bytes;
  const auto limit = route.max_conversation_bytes();
  if (current > limit || route.user_message().size() > limit - current) {
    return true;
  }
  auto remaining = limit - current - route.user_message().size();
  for (const auto& message : event.exchange) {
    const auto bytes = message_payload_bytes(message);
    if (bytes > remaining) {
      return true;
    }
    remaining -= bytes;
  }
  return false;
}

void PumpState::commit_completion(TurnRoute& route, CompletionEvent& event) {
  auto& conversation = *route.conversation();
  // The callback needs only the final assistant text, so capture it before the
  // exchange moves into the Conversation rather than retaining a second copy.
  event.text = completion_text(event);
  append_history(conversation, route.user_message(), std::move(event.exchange));
  event.exchange.clear();
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
  // Every pending event was retained because its route could consume it. Only a
  // tool call can lose that claim afterwards, by reaching a terminal state or a
  // failed dispatch.
  std::erase_if(pending_callbacks_, [this](const auto& event) {
    const auto route = find_route(event_turn_id(event.event));
    const auto discard = !route || !route->has_callback(event.event);
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
