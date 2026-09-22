#include "runtime/pump.hpp"

#include <algorithm>
#include <cassert>
#include <ranges>

namespace scry::detail {
namespace {

[[nodiscard]] std::string completion_text(const CompletionEvent& event) {
  if (event.transcript.empty()) {
    return {};
  }
  const auto& final_message = event.transcript.back();
  std::string text;
  for (const auto& block : final_message.content) {
    if (const auto* value = std::get_if<TextBlock>(&block)) {
      text += value->text;
    }
  }
  return text;
}

// Appends the turn's whole transcript - its user message, every tool round, and
// the final assistant reply - to the Conversation's history. The history block
// is shared with in-flight request snapshots, so it is copied first whenever
// anyone else still holds a reference.
void append_history(ConversationState& conversation,
                    std::vector<Message>&& transcript) {
  if (conversation.messages.use_count() > 1) {
    conversation.messages =
        std::make_shared<std::vector<Message>>(*conversation.messages);
  }
  auto& messages = *conversation.messages;
  messages.reserve(messages.size() + transcript.size());
  for (auto& message : transcript) {
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
  routes_.emplace(route->turn_id_, std::move(route));
}

TurnRoute* PumpState::find_route(const TurnId turn_id) const noexcept {
  const auto found = routes_.find(turn_id);
  return found == routes_.end() ? nullptr : found->second.get();
}

std::size_t PumpState::live_route_count() const noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(routes_ | std::views::values,
                            [](const auto& route) { return !route->terminal_; }));
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
  bool out_of_time = ingest_events(deadline);
  while (delivered < options.max_callbacks) {
    // The first deliverable callback goes out without consulting the deadline,
    // for the same reason the first event is ingested without one.
    if (delivered > 0 && clock_() >= deadline) {
      // Only work the pump could still have delivered counts as time lost, and
      // that question is asked once, here, instead of before every delivery.
      out_of_time = out_of_time || has_deliverable();
      break;
    }
    // deliver_one drops every entry it walks past that can never be delivered,
    // so a false return means nothing is left to deliver at all.
    if (!deliver_one(delivered)) {
      break;
    }
  }
  // Releasing discarded events walks every pending event, so it is skipped when
  // the time budget ran out; the next update() picks it up. Route cleanup is a
  // single pass over the routes and always runs, so a finished turn drops its
  // host captures even when every update is out of time.
  if (!out_of_time) {
    release_discarded();
  }
  clean_routes();
  const auto hit_callback_limit =
      delivered == options.max_callbacks && has_deliverable();
  return UpdateStats{
      .callbacks_delivered = delivered,
      .events_remaining = pending_callbacks_.size() + events_->size(),
      .budget_exhausted = out_of_time || hit_callback_limit,
  };
}

void PumpState::shutdown() noexcept {
  for (const auto& route : routes_ | std::views::values) {
    route->conversation_->busy = false;
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
  // budget can never starve the pump. One try_pop per iteration both asks
  // whether an event is there and takes it, so draining the queue costs one
  // lock per event rather than two.
  bool ingested = false;
  while (true) {
    if (ingested && clock_() >= deadline) {
      // A queue the loop drained exactly is not an exhausted budget: only
      // events left behind for the next update() are time lost.
      return events_->size() != 0;
    }
    auto event = events_->try_pop();
    if (!event) {
      return false;
    }
    ingested = true;
    accept_event(std::move(*event));
  }
}

void PumpState::accept_event(WorkerEvent event) {
  // Every release below credits the size measured here on arrival. Remeasuring
  // later would under-credit a committed completion, whose transcript has by
  // then moved into the Conversation, and strand the remainder in the queue's
  // per-turn byte ledger.
  const auto turn_id = event_turn_id(event);
  const auto accounted_bytes = event_payload_bytes(event);
  auto* const route = find_route(turn_id);
  if (!route || route->terminal_) {
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
  // Only a new entry counts: coalescing above merges into one the route was
  // already charged for.
  ++route->pending_events_;
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
    route.conversation_->busy = false;
    route.terminal_ = true;
  }
}

bool PumpState::conversation_limit_exceeded(
    const TurnRoute& route, const CompletionEvent& event) const noexcept {
  const auto current = route.conversation_->payload_bytes;
  const auto limit = route.max_conversation_bytes_;
  if (current > limit) {
    return true;
  }
  auto remaining = limit - current;
  for (const auto& message : event.transcript) {
    const auto bytes = message_payload_bytes(message);
    if (bytes > remaining) {
      return true;
    }
    remaining -= bytes;
  }
  return false;
}

void PumpState::commit_completion(TurnRoute& route, CompletionEvent& event) {
  // The callback needs only the final assistant text, so capture it before the
  // transcript moves into the Conversation rather than retaining a second copy.
  event.text = completion_text(event);
  append_history(*route.conversation_, std::move(event.transcript));
}

bool PumpState::deliver_one(std::size_t& callbacks_delivered) {
  auto entry = pending_callbacks_.begin();
  while (entry != pending_callbacks_.end()) {
    // find_route() explains why the route outlives the invoke below.
    auto* const route = find_route(event_turn_id(entry->event));
    if (route && route->has_callback(entry->event)) {
      auto pending = std::move(*entry);
      pending_callbacks_.erase(entry);
      release_entry(route, pending);
      ++callbacks_delivered;
      route->invoke(pending.event);
      return true;
    }
    // Every input to has_callback is monotonic - callbacks are only ever
    // cleared, the terminal and dispatch-failure flags only ever go true, and a
    // route that has left the map never returns - so this entry can never
    // become deliverable. Dropping it here returns its bytes at once instead of
    // walking past it on every later delivery.
    release_entry(route, *entry);
    entry = pending_callbacks_.erase(entry);
  }
  return false;
}

bool PumpState::has_deliverable() const noexcept {
  return std::ranges::any_of(pending_callbacks_, [this](const auto& event) {
    const auto* const route = find_route(event_turn_id(event.event));
    return route && route->has_callback(event.event);
  });
}

// Returns a dropped entry's bytes to the queue ledger and its count to the route.
void PumpState::release_entry(TurnRoute* const route, const PendingCallback& entry) {
  events_->release(event_turn_id(entry.event), entry.accounted_bytes);
  if (route) {
    // Every retained entry was counted once when accepted.
    assert(route->pending_events_ != 0);
    --route->pending_events_;
  }
}

void PumpState::release_discarded() {
  // Every pending event was retained because its route could consume it. Two
  // things revoke that claim afterwards: a tool call whose route reached a
  // terminal state or failed a dispatch, and any event on a route the host
  // disconnected.
  std::erase_if(pending_callbacks_, [this](const auto& entry) {
    auto* const route = find_route(event_turn_id(entry.event));
    const auto discard = !route || !route->has_callback(entry.event);
    if (discard) {
      release_entry(route, entry);
    }
    return discard;
  });
}

// One pass over the routes, no inner scan: the per-route pending count already
// answers whether anything still references it. A finished route retires - its
// host captures and tool snapshot go - as soon as nothing is owed to it, and it
// leaves the map once the host has dropped its handle too.
void PumpState::clean_routes() {
  std::erase_if(routes_, [](const auto& entry) {
    const auto& route = entry.second;
    if (!route->finished() || route->pending_events_ != 0) {
      return false;
    }
    route->retire();
    return !route->attached_;
  });
}

} // namespace scry::detail
