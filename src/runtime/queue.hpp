#pragma once

#include "runtime/worker_messages.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace scry::detail {

// The bytes an event holds against its turn's queue budget.
[[nodiscard]] std::size_t event_payload_bytes(const WorkerEvent& event) noexcept;

struct TurnIdHash {
  [[nodiscard]] std::size_t operator()(const TurnId id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value);
  }
};

class CommandQueue final {
public:
  void push(WorkerCommand command);
  [[nodiscard]] std::optional<WorkerCommand> try_pop();
  [[nodiscard]] std::optional<WorkerCommand> wait_pop(const std::stop_token& stopped);
  [[nodiscard]] std::optional<WorkerCommand>
  wait_pop_until(const std::stop_token& stopped, MachineTimePoint deadline);
  [[nodiscard]] std::size_t size() const;

private:
  // Under the caller's lock, with at least one command queued.
  [[nodiscard]] WorkerCommand take_front();

  mutable std::mutex mutex_{};
  std::condition_variable_any ready_{};
  std::deque<WorkerCommand> values_{};
};

class EventQueue final {
public:
  [[nodiscard]] bool push(WorkerEvent event, std::size_t max_bytes_per_turn);
  // Admits every event of one turn's non-empty batch, or none of them.
  [[nodiscard]] bool push_batch(std::vector<WorkerEvent> events,
                                std::size_t max_bytes_per_turn);
  void release(const WorkerEvent& event);
  void release(TurnId turn_id, std::size_t payload_bytes);

  [[nodiscard]] std::optional<WorkerEvent> try_pop();
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool wait_for_data(std::chrono::milliseconds timeout);

private:
  [[nodiscard]] bool coalesce_delta(const TextDeltaEvent& event,
                                    std::size_t max_bytes_per_turn);
  // Charges one turn's ledger when its queued bytes can still take the payload,
  // under the caller's lock. A refusal leaves the map exactly as it found it, so
  // a rejected push never creates the entry that release()'s erase-at-zero
  // invariant would then have to clear.
  [[nodiscard]] bool charge(TurnId turn_id, std::size_t payload_bytes,
                            std::size_t max_bytes_per_turn);

  mutable std::mutex mutex_{};
  std::condition_variable ready_{};
  std::deque<WorkerEvent> values_{};
  std::unordered_map<TurnId, std::size_t, TurnIdHash> bytes_by_turn_{};
};

} // namespace scry::detail
