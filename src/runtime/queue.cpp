#include "runtime/queue.hpp"

#include <algorithm>
#include <cassert>
#include <type_traits>
#include <utility>

namespace scry::detail {

std::size_t event_payload_bytes(const WorkerEvent& event) noexcept {
  return std::visit(
      [](const auto& value) -> std::size_t {
        using Event = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Event, TextDeltaEvent>) {
          return value.text.size();
        } else if constexpr (std::is_same_v<Event, ToolCallEvent>) {
          return content_payload_bytes(value.call);
        } else if constexpr (std::is_same_v<Event, CompletionEvent>) {
          // The transcript and the calls dropped at the tool-round limit were
          // already reserved against the Conversation budget by the machine, and
          // the completion only transfers their ownership to the host; the queue
          // charges delivery buffering only.
          return value.provider_request_id.size();
        } else if constexpr (std::is_same_v<Event, ErrorEvent>) {
          return saturating_payload_add(
              saturating_payload_add(
                  saturating_payload_add(value.error.message.size(),
                                         value.error.provider_detail.size()),
                  value.error.provider_request_id.size()),
              value.error.model_message.size());
        } else {
          return 0;
        }
      },
      event);
}

void CommandQueue::push(WorkerCommand command) {
  {
    const std::scoped_lock lock{mutex_};
    values_.push_back(std::move(command));
  }
  ready_.notify_one();
}

WorkerCommand CommandQueue::take_front() {
  auto command = std::move(values_.front());
  values_.pop_front();
  return command;
}

std::optional<WorkerCommand> CommandQueue::try_pop() {
  const std::scoped_lock lock{mutex_};
  if (values_.empty()) {
    return std::nullopt;
  }
  return take_front();
}

std::optional<WorkerCommand> CommandQueue::wait_pop(const std::stop_token& stopped) {
  std::unique_lock lock{mutex_};
  if (!ready_.wait(lock, stopped, [this] { return !values_.empty(); })) {
    return std::nullopt;
  }
  return take_front();
}

std::optional<WorkerCommand>
CommandQueue::wait_pop_until(const std::stop_token& stopped,
                             const MachineTimePoint deadline) {
  std::unique_lock lock{mutex_};
  if (!ready_.wait_until(lock, stopped, deadline,
                         [this] { return !values_.empty(); })) {
    return std::nullopt;
  }
  return take_front();
}

std::size_t CommandQueue::size() const {
  const std::scoped_lock lock{mutex_};
  return values_.size();
}

// One lookup decides and applies the charge.
bool EventQueue::charge(const TurnId turn_id, const std::size_t payload_bytes,
                        const std::size_t max_bytes_per_turn) {
  const auto found = bytes_by_turn_.find(turn_id);
  const auto queued_bytes =
      found == bytes_by_turn_.end() ? std::size_t{0} : found->second;
  if (queued_bytes > max_bytes_per_turn ||
      payload_bytes > max_bytes_per_turn - queued_bytes) {
    return false;
  }
  if (found == bytes_by_turn_.end()) {
    bytes_by_turn_.emplace(turn_id, payload_bytes);
  } else {
    found->second = queued_bytes + payload_bytes;
  }
  return true;
}

bool EventQueue::coalesce_delta(const TextDeltaEvent& event,
                                const std::size_t max_bytes_per_turn) {
  if (values_.empty()) {
    return false;
  }
  auto* previous = std::get_if<TextDeltaEvent>(&values_.back());
  if (previous == nullptr || previous->turn_id != event.turn_id) {
    return false;
  }
  if (!charge(event.turn_id, event.text.size(), max_bytes_per_turn)) {
    return false;
  }
  previous->text += event.text;
  return true;
}

bool EventQueue::push(WorkerEvent event, const std::size_t max_bytes_per_turn) {
  {
    const std::scoped_lock lock{mutex_};
    if (const auto* delta = std::get_if<TextDeltaEvent>(&event);
        delta != nullptr && coalesce_delta(*delta, max_bytes_per_turn)) {
      return true;
    }

    const auto turn_id = event_turn_id(event);
    if (!charge(turn_id, event_payload_bytes(event), max_bytes_per_turn)) {
      return false;
    }
    values_.push_back(std::move(event));
  }
  ready_.notify_one();
  return true;
}

bool EventQueue::push_batch(std::vector<WorkerEvent> events,
                            const std::size_t max_bytes_per_turn) {
  assert(!events.empty());
  const auto turn_id = event_turn_id(events.front());
  std::size_t payload_bytes = 0;
  for (const auto& event : events) {
    assert(event_turn_id(event) == turn_id);
    payload_bytes = saturating_payload_add(payload_bytes, event_payload_bytes(event));
  }
  {
    const std::scoped_lock lock{mutex_};
    if (!charge(turn_id, payload_bytes, max_bytes_per_turn)) {
      return false;
    }
    for (auto& event : events) {
      values_.push_back(std::move(event));
    }
  }
  ready_.notify_one();
  return true;
}

void EventQueue::release(const WorkerEvent& event) {
  release(event_turn_id(event), event_payload_bytes(event));
}

void EventQueue::release(const TurnId turn_id, const std::size_t payload_bytes) {
  const std::scoped_lock lock{mutex_};
  const auto found = bytes_by_turn_.find(turn_id);
  if (found == bytes_by_turn_.end()) {
    return;
  }
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

bool EventQueue::wait_for_data(const std::chrono::milliseconds timeout) {
  std::unique_lock lock{mutex_};
  return ready_.wait_for(lock, timeout, [this] { return !values_.empty(); });
}

} // namespace scry::detail
