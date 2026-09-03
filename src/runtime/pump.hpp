#pragma once

#include "runtime/queue.hpp"
#include "runtime/state.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <scry/events.hpp>
#include <scry/unique_function.hpp>
#include <string>
#include <unordered_map>

namespace scry::detail {

struct TurnRouteOptions {
  FrozenToolEntries tools{};
  std::size_t max_tool_result_bytes{};
  std::size_t max_exchange_bytes{std::numeric_limits<std::size_t>::max()};
  std::size_t max_conversation_bytes{};
  TurnCallbacks callbacks{};
};

class TurnRoute final {
public:
  TurnRoute(TurnId turn_id, std::shared_ptr<std::atomic<bool>> cancelled,
            std::weak_ptr<CommandQueue> commands,
            std::shared_ptr<ConversationState> conversation, std::string user_message,
            TurnRouteOptions options);

  [[nodiscard]] TurnId id() const noexcept;
  [[nodiscard]] std::shared_ptr<std::atomic<bool>> cancel_flag() const noexcept;
  [[nodiscard]] bool cancel() noexcept;
  [[nodiscard]] bool disconnect() noexcept;

  void detach() noexcept;
  [[nodiscard]] bool attached() const noexcept;
  [[nodiscard]] bool terminal() const noexcept;
  [[nodiscard]] bool finished() const noexcept;
  void mark_terminal() noexcept;

  [[nodiscard]] bool has_callback(const WorkerEvent& event) const noexcept;
  void invoke(const WorkerEvent& event);

  [[nodiscard]] const std::shared_ptr<ConversationState>& conversation() const noexcept;
  [[nodiscard]] const std::string& user_message() const noexcept;
  [[nodiscard]] std::size_t max_conversation_bytes() const noexcept;

private:
  void dispatch(const ToolCallEvent& event);
  void notify_tool_observer(const ToolCallBlock& call, const ToolResultBlock& result);

  TurnId turn_id_{};
  std::shared_ptr<std::atomic<bool>> cancelled_{};
  std::weak_ptr<CommandQueue> commands_{};
  std::shared_ptr<ConversationState> conversation_{};
  std::string user_message_{};
  FrozenToolEntries tools_{};
  std::size_t max_tool_result_bytes_{};
  std::size_t remaining_exchange_bytes_{std::numeric_limits<std::size_t>::max()};
  std::size_t max_conversation_bytes_{};
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
  [[nodiscard]] std::shared_ptr<TurnRoute> find_route(TurnId turn_id) const;
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
  void release_discarded();
  void clean_routes();

  std::shared_ptr<EventQueue> events_{};
  PumpClock clock_{};
  std::unordered_map<TurnId, std::shared_ptr<TurnRoute>, TurnIdHash> routes_{};
  std::deque<PendingCallback> pending_callbacks_{};
  bool updating_{false};
};

} // namespace scry::detail
