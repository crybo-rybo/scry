/// @file
/// @brief Synchronized FIFO queues forming the runtime's message-passing boundary.
///
/// CommandQueue is a conventional blocking FIFO used by the worker actor. EventQueue
/// adds per-turn byte accounting, atomic tool-call batches, and adjacent text
/// coalescing so a stalled main-loop pump remains bounded without exposing partial
/// provider batches.

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

/// Hash adapter for using TurnId as an unordered-container key.
struct TurnIdHash {
  /// Hashes the identifier's stable integer representation.
  ///
  /// @param id Turn identifier to hash.
  /// @return Standard-library hash of TurnId::value.
  [[nodiscard]] std::size_t operator()(const TurnId id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value);
  }
};

/// Mutex-protected multi-producer FIFO with stop-aware worker waits.
///
/// Values move across the synchronization boundary. The queue owns no thread and makes
/// no shutdown decision; a caller-supplied stop token interrupts blocking waits.
///
/// @tparam Value Move-constructible command value stored by the queue.
template <typename Value> class BlockingQueue final {
public:
  /// Appends one value and wakes a waiter.
  ///
  /// @param value Value transferred to the back of the FIFO.
  void push(Value value) {
    {
      const std::scoped_lock lock{mutex_};
      values_.push_back(std::move(value));
    }
    ready_.notify_one();
  }

  /// Removes the oldest value without blocking.
  ///
  /// @return The moved front value, or std::nullopt when the queue is empty.
  [[nodiscard]] std::optional<Value> try_pop() {
    const std::scoped_lock lock{mutex_};
    if (values_.empty()) {
      return std::nullopt;
    }
    auto value = std::move(values_.front());
    values_.pop_front();
    return value;
  }

  /// Waits for the oldest value or cooperative shutdown.
  ///
  /// @param stopped Harness-worker shutdown token.
  /// @return The moved front value, or std::nullopt when stop wins before data arrives.
  [[nodiscard]] std::optional<Value> wait_pop(const std::stop_token& stopped) {
    std::unique_lock lock{mutex_};
    if (!ready_.wait(lock, stopped, [this] { return !values_.empty(); })) {
      return std::nullopt;
    }
    auto value = std::move(values_.front());
    values_.pop_front();
    return value;
  }

  /// Waits for the oldest value until a deadline or cooperative shutdown.
  ///
  /// @tparam Clock Deadline clock type accepted by condition_variable_any.
  /// @tparam Duration Deadline duration type.
  /// @param stopped Harness-worker shutdown token.
  /// @param deadline Absolute wake deadline.
  /// @return Front value, or std::nullopt when timeout or stop wins.
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

  /// Returns a synchronized snapshot of the queued item count.
  ///
  /// @return Number of values currently waiting in the FIFO.
  [[nodiscard]] std::size_t size() const {
    const std::scoped_lock lock{mutex_};
    return values_.size();
  }

private:
  /// Protects both the FIFO and its wake predicate.
  mutable std::mutex mutex_{};
  /// Stop-token-aware notification channel for worker waits.
  std::condition_variable_any ready_{};
  /// Queued values in publication order.
  std::deque<Value> values_{};
};

/// App-to-worker command FIFO.
using CommandQueue = BlockingQueue<WorkerCommand>;

/// Bounded worker-to-pump event FIFO with per-turn accounting.
///
/// Removing an event with try_pop() does not release its byte charge: the pump may
/// retain it until a later update. The charge is returned explicitly only after
/// delivery or discard, keeping both queued and pump-staged payload inside one limit.
class EventQueue final {
public:
  /// Publishes one event if it fits the turn's remaining queue budget.
  ///
  /// An adjacent TextDeltaEvent for the same turn may be merged in place. The operation
  /// is all-or-nothing and wakes one waiting pump/consumer on insertion.
  ///
  /// @param event Event transferred on success.
  /// @param max_bytes_per_turn Payload ceiling for this turn.
  /// @return true on insertion/coalescing; false when the byte bound is exceeded.
  [[nodiscard]] bool push(WorkerEvent event, std::size_t max_bytes_per_turn);
  /// Atomically publishes a same-turn event batch.
  ///
  /// No event becomes visible unless every member shares one TurnId and their combined
  /// payload fits. This is the publication primitive for provider tool-call batches.
  ///
  /// @param events Ordered same-turn events transferred on success.
  /// @param max_bytes_per_turn Payload ceiling for the batch's turn.
  /// @return true for an empty or fully inserted batch; false with the queue unchanged.
  [[nodiscard]] bool push_batch(std::vector<WorkerEvent> events,
                                std::size_t max_bytes_per_turn);
  /// Publishes a terminal event against the caller's reserved full byte ceiling.
  ///
  /// The worker keeps capacity out of ordinary-event admission so a bounded terminal
  /// outcome can always be represented. This operation still validates the supplied
  /// ceiling and never evicts earlier events.
  ///
  /// @param event Terminal event transferred on success.
  /// @param max_bytes_per_turn Full configured per-turn payload ceiling.
  /// @return true when published; false if the reserved bound was insufficient.
  [[nodiscard]] bool push_terminal(WorkerEvent event, std::size_t max_bytes_per_turn);
  /// Returns one event's measured payload charge to its turn ledger.
  ///
  /// @param event Event that has been delivered or discarded.
  void release(const WorkerEvent& event);
  /// Returns an explicitly retained payload charge to one turn ledger.
  ///
  /// This overload is used after an event has been moved or coalesced and can no longer
  /// be measured reliably from its current value. Over-release saturates at zero.
  ///
  /// @param turn_id Turn whose accounting is credited.
  /// @param payload_bytes Previously recorded byte charge to release.
  void release(TurnId turn_id, std::size_t payload_bytes);

  /// Removes the oldest event without releasing its byte charge.
  ///
  /// @return The moved front event, or std::nullopt when no event is queued.
  [[nodiscard]] std::optional<WorkerEvent> try_pop();
  /// Returns a synchronized snapshot of the queued event count.
  ///
  /// Pump-staged events are absent from this count but remain in the byte ledger.
  ///
  /// @return Number of events still in the queue FIFO.
  [[nodiscard]] std::size_t size() const;
  /// Waits up to a relative timeout for at least one queued event.
  ///
  /// The operation observes but does not consume data.
  ///
  /// @param timeout Maximum duration to wait.
  /// @return true when data is available; false on timeout.
  [[nodiscard]] bool wait_for_data(std::chrono::milliseconds timeout);

private:
  /// Merges an adjacent same-turn delta while the queue mutex is held.
  ///
  /// @param event Delta whose text remains caller-owned unless merging succeeds.
  /// @param max_bytes_per_turn Payload ceiling for the event's turn.
  /// @return true when the queued tail absorbed the payload.
  [[nodiscard]] bool coalesce_delta(const TextDeltaEvent& event,
                                    std::size_t max_bytes_per_turn);

  /// Protects the FIFO and all accounting state.
  mutable std::mutex mutex_{};
  /// Notification channel for consumers waiting on nonempty @ref values_.
  std::condition_variable ready_{};
  /// Worker events in publication order.
  std::deque<WorkerEvent> values_{};
  /// Payload retained by the queue or pump staging area, grouped by turn.
  std::unordered_map<TurnId, std::size_t, TurnIdHash> bytes_by_turn_{};
};

} // namespace scry::detail
