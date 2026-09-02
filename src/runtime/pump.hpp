/// @file
/// @brief Main-loop event routing, callback delivery, and transactional commit state.
///
/// The pump is the runtime's app-thread boundary. It stages worker events under a soft
/// update budget, dispatches tools and observers on the caller's thread, commits
/// complete exchanges atomically, and releases queue accounting only when payload is no
/// longer retained.

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

/// Accepted-turn data transferred into a pump-side TurnRoute.
///
/// Options are fixed at acceptance before the corresponding worker command becomes
/// visible, making callbacks, handlers, and resource limits immutable for the turn.
struct TurnRouteOptions {
  /// Pump-owned immutable registration snapshot captured for this turn.
  FrozenToolEntries tools{};
  /// Maximum canonical bytes allowed for one tool result.
  std::size_t max_tool_result_bytes{};
  /// Remaining cumulative assistant/tool exchange budget after admission.
  std::size_t max_exchange_bytes{std::numeric_limits<std::size_t>::max()};
  /// Maximum committed payload bytes allowed in the Conversation.
  std::size_t max_conversation_bytes{};
  /// Complete callback set attached atomically at acceptance.
  TurnCallbacks callbacks{};
};

/// Pump-side owner of one accepted turn's callbacks, handlers, and commit route.
///
/// A route outlives a dropped Turn handle and retains the Conversation until terminal
/// processing. It never owns worker state: all worker interaction uses immutable
/// TurnId, CommandQueue messages, and the shared cancellation atomic. Route operations
/// are app-thread confined; only that atomic is read by the worker.
class TurnRoute final {
public:
  /// Creates a fully attached route before the worker can observe the turn.
  ///
  /// @param turn_id Immutable accepted-turn identifier.
  /// @param cancelled Per-turn cancellation atomic shared with the worker.
  /// @param commands Weak command path, allowing handles to outlive the Harness safely.
  /// @param conversation Conversation state retained through terminal processing.
  /// @param user_message Original user text committed only on success.
  /// @param options Snapshotted handlers, limits, and callbacks.
  TurnRoute(TurnId turn_id, std::shared_ptr<std::atomic<bool>> cancelled,
            std::weak_ptr<CommandQueue> commands,
            std::shared_ptr<ConversationState> conversation, std::string user_message,
            TurnRouteOptions options);

  /// Returns the immutable correlation identity.
  ///
  /// @return Turn identifier assigned at acceptance.
  [[nodiscard]] TurnId id() const noexcept;
  /// Shares the sanctioned per-turn cancellation atomic with a Turn handle.
  ///
  /// @return Strong reference to the cancellation flag.
  [[nodiscard]] std::shared_ptr<std::atomic<bool>> cancel_flag() const noexcept;
  /// Requests cooperative cancellation exactly once while nonterminal.
  ///
  /// The first request sets the atomic and, when the command queue still exists,
  /// publishes CancelTurnCommand so queued and waiting turns wake without issuing
  /// further I/O.
  ///
  /// @return true only when this call changed the flag and issued the request.
  [[nodiscard]] bool cancel() noexcept;

  /// Records that the public handle was dropped without affecting turn execution.
  void detach() noexcept;
  /// Reports whether a public Turn handle still observes this route.
  ///
  /// @return true until the handle implementation detaches.
  [[nodiscard]] bool attached() const noexcept;
  /// Reports whether terminal commit/rollback processing has occurred.
  ///
  /// @return true after the first completion, error, or cancellation event.
  [[nodiscard]] bool terminal() const noexcept;
  /// Latches the route terminal so later events become dead on arrival.
  void mark_terminal() noexcept;

  /// Classifies whether this route can still consume an event.
  ///
  /// Empty user observers reject their matching notification immediately. ToolCallEvent
  /// is considered consumable by the route's internal dispatcher even without an
  /// on_tool_call observer, unless terminal state or a prior fatal dispatch suppresses
  /// it.
  ///
  /// @param event Worker event considered for staging or delivery.
  /// @return true when invoke() has meaningful work for the event.
  [[nodiscard]] bool has_callback(const WorkerEvent& event) const noexcept;
  /// Performs the route action for one previously accepted event.
  ///
  /// Text and terminal events invoke application callbacks; tool calls run the
  /// snapshotted handler and publish its result. Callback/observer exceptions propagate
  /// synchronously to Harness::update() after the event has been counted as delivered.
  ///
  /// @param event Event whose TurnId matches this route.
  void invoke(const WorkerEvent& event);

  /// Exposes retained Conversation state to terminal pump processing.
  ///
  /// @return Shared state reference owned by this route.
  [[nodiscard]] const std::shared_ptr<ConversationState>& conversation() const noexcept;
  /// Returns the original user text reserved at admission.
  ///
  /// @return Borrowed string retained for possible successful commit.
  [[nodiscard]] const std::string& user_message() const noexcept;
  /// Returns the Conversation payload ceiling captured at acceptance.
  ///
  /// @return Maximum committed semantic payload bytes.
  [[nodiscard]] std::size_t max_conversation_bytes() const noexcept;

private:
  /// Executes one tool call and returns its result to the serialized worker turn.
  ///
  /// Dispatch rechecks cancellation before and after non-preemptive user code, reserves
  /// the canonical result against the cumulative exchange budget, and latches fatal
  /// dispatch failure so the remainder of the atomic provider batch is suppressed.
  ///
  /// @param event Tool call and authoritative remaining-budget observation.
  void dispatch(const ToolCallEvent& event);
  /// Invokes the optional post-result tool observer with a borrowed public view.
  ///
  /// @param call Provider-neutral call whose result has already been posted.
  void notify_tool_observer(const ToolCallBlock& call);

  /// Immutable accepted-turn identifier.
  TurnId turn_id_{};
  /// Shared per-turn cancellation signal; the only mutable route state seen by worker
  /// I/O.
  std::shared_ptr<std::atomic<bool>> cancelled_{};
  /// Non-owning path for cancellation and tool-result commands.
  std::weak_ptr<CommandQueue> commands_{};
  /// Pump-side Conversation lifetime retained through terminalization.
  std::shared_ptr<ConversationState> conversation_{};
  /// User text withheld from committed history until successful terminal delivery.
  std::string user_message_{};
  /// Immutable accepted-turn registrations, including pump-only handlers.
  FrozenToolEntries tools_{};
  /// Per-result serialized payload ceiling.
  std::size_t max_tool_result_bytes_{};
  /// Pump-side view of the machine's remaining cumulative exchange budget.
  std::size_t remaining_exchange_bytes_{std::numeric_limits<std::size_t>::max()};
  /// Conversation payload ceiling rechecked immediately before commit.
  std::size_t max_conversation_bytes_{};
  /// Whether the public Turn handle remains attached.
  bool attached_{true};
  /// Whether commit/rollback and busy-state release have occurred.
  bool terminal_{false};
  /// Whether a fatal framework dispatch error suppresses the remaining batch suffix.
  bool tool_dispatch_failed_{false};
  /// Immutable callback set installed before worker command publication.
  TurnCallbacks callbacks_{};
};

/// Move-only clock source injected into PumpState for deterministic budget tests.
using PumpClock = UniqueFunction<std::chrono::steady_clock::time_point()>;

/// App-thread state machine for event staging, delivery, and route cleanup.
///
/// PumpState is deliberately separate from the worker's sans-I/O TurnMachine. It owns
/// callbacks and Conversation mutation, neither of which may cross the worker boundary.
/// One non-reentrant update() call drains/stages events and invokes work under caller
/// limits; undelivered events remain accounted and roll into a later call.
class PumpState final {
public:
  /// Creates a pump over the shared worker event queue.
  ///
  /// @param events Worker-to-pump queue shared with one Harness actor.
  /// @param clock Optional deterministic steady-clock source; defaults to steady_clock.
  explicit PumpState(std::shared_ptr<EventQueue> events, PumpClock clock = {});

  /// Installs a fully initialized accepted-turn route.
  ///
  /// @param route Route inserted before its worker command becomes visible.
  void add_route(std::shared_ptr<TurnRoute> route);
  /// Finds pump-owned state for one accepted turn.
  ///
  /// @param turn_id Immutable correlation identifier.
  /// @return Shared route, or nullptr when it has already been cleaned up.
  [[nodiscard]] std::shared_ptr<TurnRoute> find_route(TurnId turn_id) const;
  /// Counts accepted routes that have not reached terminal pump processing.
  ///
  /// @return Live turn count used for Harness admission control.
  [[nodiscard]] std::size_t live_route_count() const noexcept;
  /// Reports whether callback delivery is currently active.
  ///
  /// @return true only during the dynamic extent of update().
  [[nodiscard]] bool updating() const noexcept;

  /// Pumps worker events into app-thread actions under soft caller limits.
  ///
  /// A reentrant call performs no work and reports budget exhaustion. Time is checked
  /// between callbacks, never by preempting application code. Any exception thrown by
  /// an application callback propagates after that event has been removed and counted.
  ///
  /// @param options Soft time and callback-count limits for this invocation.
  /// @return Counts and remaining-work information for the host main loop.
  [[nodiscard]] UpdateStats update(UpdateOptions options);
  /// Abandons every route and queued callback during Harness destruction.
  ///
  /// Conversation busy flags and event-ledger charges are released, but no callback or
  /// tool handler is invoked.
  void shutdown() noexcept;

private:
  /// Pump-staged event paired with its immutable queue-ledger charge.
  ///
  /// Payload may move or coalesce after ingestion, so release uses the captured byte
  /// count rather than remeasuring the event later.
  struct PendingCallback {
    /// Event retained for a future app-thread action.
    WorkerEvent event{};
    /// Original cumulative bytes to return on delivery or discard.
    std::size_t accounted_bytes{};
  };

  /// Moves queued events into pump staging until the update deadline.
  ///
  /// @param deadline Absolute soft deadline for this update call.
  /// @return true when ingestion stopped because the deadline was reached.
  [[nodiscard]] bool ingest_events(std::chrono::steady_clock::time_point deadline);
  /// Applies terminal semantics and either stages or immediately releases one event.
  ///
  /// @param event Newly dequeued worker event transferred to the pump.
  void accept_event(WorkerEvent event);
  /// Appends a delta to an already staged same-turn text callback.
  ///
  /// @param event Newly ingested text fragment.
  /// @param accounted_bytes Queue-ledger charge that must follow the merged payload.
  /// @return true when an existing pending callback absorbed the event.
  [[nodiscard]] bool coalesce_pending_delta(const TextDeltaEvent& event,
                                            std::size_t accounted_bytes);
  /// Performs exactly-once terminal state changes before callback retention is tested.
  ///
  /// A valid completion commits the exchange; every terminal alternative clears busy
  /// and marks the route. An oversized completion is converted to ErrorEvent with no
  /// commit.
  ///
  /// @param route Matching live route.
  /// @param event Event that may be converted or moved during terminal processing.
  void apply_terminal(TurnRoute& route, WorkerEvent& event);
  /// Checks the complete pending commit against the Conversation payload ceiling.
  ///
  /// @param route Route containing current history and original user text.
  /// @param event Successful exchange proposed for commit.
  /// @return true when any addition would cross the configured bound.
  [[nodiscard]] bool
  conversation_limit_exceeded(const TurnRoute& route,
                              const CompletionEvent& event) const noexcept;
  /// Atomically appends user text and the full exchange to committed history.
  ///
  /// Final assistant text is captured for callback delivery before the exchange moves.
  ///
  /// @param route Route owning the Conversation and user text.
  /// @param event Completion whose exchange is consumed.
  void commit_completion(TurnRoute& route, CompletionEvent& event);
  /// Delivers one still-consumable pending event.
  ///
  /// Queue accounting is released and @p callbacks_delivered is incremented before user
  /// code runs, preserving consistent state if a callback throws.
  ///
  /// @param callbacks_delivered Counter updated on invocation.
  /// @return true when one event was delivered; false when no route can consume one.
  [[nodiscard]] bool deliver_one(std::size_t& callbacks_delivered);
  /// Reports whether any staged event still has a consuming route action.
  ///
  /// @return true when deliver_one() can make progress.
  [[nodiscard]] bool has_deliverable() const noexcept;
  /// Releases staged events invalidated by terminalization or fatal tool dispatch.
  void release_discarded();
  /// Erases detached terminal routes after all of their staged events are gone.
  void clean_routes();

  /// Shared worker-to-pump queue and payload ledger.
  std::shared_ptr<EventQueue> events_{};
  /// Steady clock used only for update budget observations.
  PumpClock clock_{};
  /// Pump-owned routes indexed by immutable TurnId.
  std::unordered_map<TurnId, std::shared_ptr<TurnRoute>, TurnIdHash> routes_{};
  /// Events retained across update calls until delivered or discarded.
  std::deque<PendingCallback> pending_callbacks_{};
  /// Reentrancy guard set for the dynamic extent of update().
  bool updating_{false};
};

} // namespace scry::detail
