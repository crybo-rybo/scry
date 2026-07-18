#include "runtime/queue.hpp"

#include <algorithm>
#include <type_traits>

namespace scry::detail {

bool EventQueue::coalesce_delta(const TextDeltaEvent& event,
                                const std::size_t max_bytes_per_turn) {
  if (values_.empty()) {
    return false;
  }
  auto* previous = std::get_if<TextDeltaEvent>(&values_.back());
  if (previous == nullptr || previous->turn_id != event.turn_id) {
    return false;
  }
  auto& queued_bytes = bytes_by_turn_[event.turn_id];
  if (queued_bytes > max_bytes_per_turn ||
      event.text.size() > max_bytes_per_turn - queued_bytes) {
    return false;
  }
  previous->text += event.text;
  queued_bytes += event.text.size();
  return true;
}

bool EventQueue::push(WorkerEvent event, const std::size_t max_bytes_per_turn) {
  const std::scoped_lock lock{mutex_};
  if (const auto* delta = std::get_if<TextDeltaEvent>(&event);
      delta != nullptr && coalesce_delta(*delta, max_bytes_per_turn)) {
    return true;
  }

  const auto turn_id = event_turn_id(event);
  const auto payload_bytes = event_payload_bytes(event);
  const auto queued_bytes = bytes_by_turn_[turn_id];
  if (queued_bytes > max_bytes_per_turn ||
      payload_bytes > max_bytes_per_turn - queued_bytes) {
    return false;
  }
  values_.push_back(std::move(event));
  bytes_by_turn_[turn_id] = queued_bytes + payload_bytes;
  return true;
}

void EventQueue::push_terminal(WorkerEvent event) {
  const std::scoped_lock lock{mutex_};
  const auto turn_id = event_turn_id(event);
  bytes_by_turn_[turn_id] += event_payload_bytes(event);
  values_.push_back(std::move(event));
}

void EventQueue::discard(const TurnId turn_id) {
  const std::scoped_lock lock{mutex_};
  std::size_t removed_bytes = 0;
  std::erase_if(values_, [turn_id, &removed_bytes](const auto& event) {
    if (event_turn_id(event) != turn_id) {
      return false;
    }
    removed_bytes += event_payload_bytes(event);
    return true;
  });
  const auto found = bytes_by_turn_.find(turn_id);
  if (found == bytes_by_turn_.end()) {
    return;
  }
  found->second -= std::min(found->second, removed_bytes);
  if (found != bytes_by_turn_.end() && found->second == 0) {
    bytes_by_turn_.erase(found);
  }
}

void EventQueue::release(const WorkerEvent& event) {
  const std::scoped_lock lock{mutex_};
  const auto found = bytes_by_turn_.find(event_turn_id(event));
  if (found == bytes_by_turn_.end()) {
    return;
  }
  const auto payload_bytes = event_payload_bytes(event);
  found->second -= std::min(found->second, payload_bytes);
  if (found->second == 0) {
    bytes_by_turn_.erase(found);
  }
}

std::optional<WorkerEvent> EventQueue::try_pop() {
  const std::scoped_lock lock{mutex_};
  if (values_.empty()) {
    return std::nullopt;
  }
  auto event = std::move(values_.front());
  values_.pop_front();
  return event;
}

std::size_t EventQueue::size() const {
  const std::scoped_lock lock{mutex_};
  return values_.size();
}

} // namespace scry::detail
