#pragma once

#include "runtime/queue.hpp"
#include "runtime/state.hpp"
#include "runtime/tool_registry_impl.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <scry/events.hpp>
#include <scry/unique_function.hpp>
#include <string>
#include <unordered_map>

namespace scry::detail {

struct TurnRouteOptions {
  FrozenToolEntries tools{};
  std::size_t max_tool_result_bytes{};
  std::size_t max_conversation_bytes{};
  std::optional<std::uint32_t> max_tool_calls{};
  TurnCallbacks callbacks{};
};

class TurnRoute final {
public:
  TurnRoute(TurnId turn_id, std::shared_ptr<std::atomic<bool>> cancelled,
            std::weak_ptr<CommandQueue> commands,
            std::shared_ptr<ConversationState> conversation, TurnRouteOptions options);

  [[nodiscard]] TurnId id() const noexcept;
  [[nodiscard]] std::shared_ptr<std::atomic<bool>> cancel_flag() const noexcept;
  [[nodiscard]] bool cancel() noexcept;
  [[nodiscard]] bool disconnect() noexcept;

  void detach() noexcept;
  [[nodiscard]] bool terminal() const noexcept;
  [[nodiscard]] bool finished() const noexcept;
  void retire() noexcept;

  [[nodiscard]] bool has_callback(const WorkerEvent& event) const noexcept;
  // Takes the event by mutable reference so the payloads it owns move into the
  // callback's value instead of being copied into it.
  void invoke(WorkerEvent& event);

private:
  // PumpState owns the route's delivery bookkeeping: the terminal and attached
  // flags, the pending-entry count, and the Conversation the commit writes to.
  friend class PumpState;

  [[nodiscard]] std::optional<Result<ToolResultBlock>>
  admit(const ToolCallEvent& event);
  [[nodiscard]] std::optional<Result<ToolResultBlock>>
  produce(const ToolCallEvent& event);
  void dispatch(ToolCallEvent& event);
  [[nodiscard]] std::optional<ToolCall>
  observation(const ToolCallEvent& event, const Result<ToolResultBlock>& result) const;
  void notify_tool_observer(ToolCallEvent& event, ToolCall& observed);

  TurnId turn_id_{};
  std::shared_ptr<std::atomic<bool>> cancelled_{};
  std::weak_ptr<CommandQueue> commands_{};
  std::shared_ptr<ConversationState> conversation_{};
  FrozenToolEntries tools_{};
  std::size_t max_tool_result_bytes_{};
  std::size_t remaining_exchange_bytes_{std::numeric_limits<std::size_t>::max()};
  std::size_t max_conversation_bytes_{};
  // One per entry the pump retains for this route, released as it drops each.
  std::size_t pending_events_{};
  std::optional<std::uint32_t> max_tool_calls_{};
  // Calls this route has taken charge of, and the subset it refused. Both are
  // route-owned: the machine counts what the model asked for, not what ran.
  std::uint32_t dispatched_count_{};
  std::uint32_t rejected_count_{};
  bool attached_{true};
  bool disconnected_{false};
  bool invoking_{false};
  bool terminal_{false};
  bool terminal_delivered_{false};
  bool tool_dispatch_failed_{false};
  TurnCallbacks callbacks_{};
};

using PumpClock = UniqueFunction<std::chrono::steady_clock::time_point()>;

class PumpState final {
public:
  explicit PumpState(std::shared_ptr<EventQueue> events, PumpClock clock = {});

  void add_route(std::shared_ptr<TurnRoute> route);
  // The pointer stays valid until the next clean_routes(), which runs only at
  // the end of update(). update() is non-reentrant and nothing a host callback
  // can call - disconnect(), cancel(), send() - erases a route, so a lookup made
  // before running a callback still names a live route after it.
  [[nodiscard]] TurnRoute* find_route(TurnId turn_id) const noexcept;
  [[nodiscard]] std::size_t live_route_count() const noexcept;
  [[nodiscard]] bool updating() const noexcept;

  [[nodiscard]] UpdateStats update(UpdateOptions options);
  void shutdown() noexcept;

private:
  struct PendingCallback {
    WorkerEvent event{};
    std::size_t accounted_bytes{};
  };

  [[nodiscard]] bool ingest_events(std::chrono::steady_clock::time_point deadline);
  void accept_event(WorkerEvent event);
  [[nodiscard]] bool coalesce_pending_delta(const TextDeltaEvent& event,
                                            std::size_t accounted_bytes);
  void apply_terminal(TurnRoute& route, WorkerEvent& event);
  [[nodiscard]] bool
  conversation_limit_exceeded(const TurnRoute& route,
                              const CompletionEvent& event) const noexcept;
  void commit_completion(TurnRoute& route, CompletionEvent& event);
  [[nodiscard]] bool deliver_one(std::size_t& callbacks_delivered);
  [[nodiscard]] bool has_deliverable() const noexcept;
  void release_entry(TurnRoute* route, const PendingCallback& entry);
  void release_discarded();
  void clean_routes();

  std::shared_ptr<EventQueue> events_{};
  PumpClock clock_{};
  std::unordered_map<TurnId, std::shared_ptr<TurnRoute>, TurnIdHash> routes_{};
  std::deque<PendingCallback> pending_callbacks_{};
  bool updating_{false};
};

} // namespace scry::detail
