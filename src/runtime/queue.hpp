#pragma once

#include "runtime/messages.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scry::detail {

struct TurnIdHash {
  [[nodiscard]] std::size_t operator()(const TurnId id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value);
  }
};

template <typename Value> class BlockingQueue final {
public:
  void push(Value value) {
    {
      const std::scoped_lock lock{mutex_};
      values_.push_back(std::move(value));
    }
    ready_.notify_one();
  }

  [[nodiscard]] std::optional<Value> try_pop() {
    const std::scoped_lock lock{mutex_};
    if (values_.empty()) {
      return std::nullopt;
    }
    auto value = std::move(values_.front());
    values_.pop_front();
    return value;
  }

  [[nodiscard]] std::optional<Value> wait_pop(const std::stop_token& stopped) {
    std::unique_lock lock{mutex_};
    if (!ready_.wait(lock, stopped, [this] { return !values_.empty(); })) {
      return std::nullopt;
    }
    auto value = std::move(values_.front());
    values_.pop_front();
    return value;
  }

  template <typename Clock, typename Duration>
  [[nodiscard]] std::optional<Value>
  wait_pop_until(const std::stop_token& stopped,
                 const std::chrono::time_point<Clock, Duration> deadline) {
    std::unique_lock lock{mutex_};
    if (!ready_.wait_until(lock, stopped, deadline,
                           [this] { return !values_.empty(); })) {
      return std::nullopt;
    }
    auto value = std::move(values_.front());
    values_.pop_front();
    return value;
  }

  [[nodiscard]] std::size_t size() const {
    const std::scoped_lock lock{mutex_};
    return values_.size();
  }

private:
  mutable std::mutex mutex_{};
  std::condition_variable_any ready_{};
  std::deque<Value> values_{};
};

using CommandQueue = BlockingQueue<WorkerCommand>;

class EventQueue final {
public:
  [[nodiscard]] bool push(WorkerEvent event, std::size_t max_bytes_per_turn);
  [[nodiscard]] bool push_batch(std::vector<WorkerEvent> events,
                                std::size_t max_bytes_per_turn);
  void push_terminal(WorkerEvent event);
  [[nodiscard]] bool push_terminal(WorkerEvent event, std::size_t max_bytes_per_turn);
  void discard(TurnId turn_id);
  void release(const WorkerEvent& event);
  void release(TurnId turn_id, std::size_t payload_bytes);

  [[nodiscard]] std::optional<WorkerEvent> try_pop();
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool wait_for_data(std::chrono::milliseconds timeout);

private:
  [[nodiscard]] bool coalesce_delta(const TextDeltaEvent& event,
                                    std::size_t max_bytes_per_turn);

  mutable std::mutex mutex_{};
  std::condition_variable ready_{};
  std::deque<WorkerEvent> values_{};
  std::unordered_map<TurnId, std::size_t, TurnIdHash> bytes_by_turn_{};
};

} // namespace scry::detail
