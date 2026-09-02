#pragma once

#include <memory>
#include <scry/config.hpp>
#include <scry/conversation.hpp>
#include <scry/error.hpp>
#include <scry/events.hpp>
#include <scry/tool_registry.hpp>
#include <scry/turn.hpp>
#include <scry/turn_id.hpp>
#include <string>

namespace scry {

namespace detail {
class HarnessTestAccess;
} // namespace detail

/// Configured runtime that owns tools, network state, worker I/O, and callback
/// delivery.
///
/// A Harness never owns the host's main loop. Asynchronous work progresses on its
/// worker while the host periodically calls update() to deliver application-facing
/// activity.
class Harness final {
public:
  /// Validates configuration and starts a Harness-owned worker.
  /// @param config Provider, retry, timeout, and resource configuration.
  /// @return A Harness, or ErrorCategory::invalid_config when validation fails.
  [[nodiscard]] static Result<Harness> create(Config config);

  /// Runs exactly the configuration checks create() runs, without initializing
  /// libcurl or starting a worker.
  ///
  /// This lets a settings dialog report a bad configuration before paying for a
  /// Harness. Success here does not guarantee create() succeeds: create() can still
  /// fail for runtime reasons such as libcurl initialization or thread start.
  /// @param config Configuration to check.
  /// @return Success, or ErrorCategory::invalid_config describing the first problem.
  [[nodiscard]] static Status validate(const Config& config);

  /// Cancels outstanding work, stops Scry-owned I/O, and joins the worker.
  ///
  /// Undelivered callbacks are discarded once destruction begins.
  ~Harness();

  /// Moves ownership of the worker and all Harness state.
  Harness(Harness&&) noexcept;

  /// Replaces this Harness with another moved Harness.
  /// @return This Harness.
  Harness& operator=(Harness&&) noexcept;

  /// Harnesses are not copyable.
  Harness(const Harness&) = delete;

  /// Harnesses are not copy-assignable.
  Harness& operator=(const Harness&) = delete;

  /// Returns the Harness-owned additive tool registry.
  /// @return Mutable registry for future turns.
  [[nodiscard]] ToolRegistry& tools() noexcept;

  /// Returns the Harness-owned additive tool registry.
  /// @return Read-only registry view.
  [[nodiscard]] const ToolRegistry& tools() const noexcept;

  /// Accepts an asynchronous user turn.
  ///
  /// The call validates admission synchronously, snapshots current tool registrations,
  /// and returns without waiting for network I/O. The host must call update() to
  /// execute app-thread tools and deliver callbacks.
  ///
  /// Callbacks are attached infallibly and atomically as the turn is accepted, so no
  /// event can precede them and there is no later registration or replay step. When
  /// TurnCallbacks::on_finished is non-empty, an accepted turn invokes it exactly once
  /// unless this Harness is destroyed first; see ~Harness().
  /// @param conversation Conversation that receives the exchange on successful
  /// completion.
  /// @param user_message User text appended transactionally if the turn succeeds.
  /// @param callbacks Optional per-turn observers delivered inside update().
  /// @return A controllable Turn handle, or an immediate admission error.
  [[nodiscard]] Result<Turn> send(Conversation& conversation, std::string user_message,
                                  TurnCallbacks callbacks = {});

  /// Requests cooperative cancellation of one accepted turn by identifier.
  ///
  /// This is Turn::cancel() addressed by id, for hosts that retain TurnId values
  /// rather than Turn handles. The contract is identical: a non-empty
  /// TurnCallbacks::on_finished still terminates the turn with an Error whose category
  /// is ErrorCategory::cancelled, unless Harness destruction begins first.
  /// @param turn_id Identifier returned by Turn::id().
  /// @return true only when this call issued the cancellation request; false when
  /// cancellation was already requested, the turn was terminal, no such turn is known
  /// to this Harness, or this Harness is moved from.
  bool cancel(TurnId turn_id) noexcept;

  /// Runs one turn synchronously on top of send() and update().
  ///
  /// This is the only public operation that waits for network I/O. It is intended for
  /// command line programs and tests rather than host-owned main loops. Three
  /// consequences follow from it being a pump loop rather than a private wait:
  /// - It pumps update() until this turn terminates, so callbacks and app-thread tool
  ///   handlers belonging to every other accepted turn run inside the call.
  /// - The waited turn cannot be cancelled by the caller, because no Turn handle is
  ///   exposed. It ends only through completion, a terminal error, or Harness
  ///   destruction.
  /// - Calling it from inside a callback is rejected with ErrorCategory::invalid_state.
  /// @param conversation Conversation that receives the exchange on success.
  /// @param user_message User text sent to the configured model.
  /// @return The successful completion or terminal error.
  [[nodiscard]] Result<Completion> send_and_wait(Conversation& conversation,
                                                 std::string user_message);

  /// Pumps queued events, app-thread tool handlers, and callbacks on the calling
  /// thread.
  ///
  /// Reentrant calls are rejected and report UpdateStats::budget_exhausted. Exceptions
  /// escaping an application callback propagate synchronously after the event counts as
  /// delivered; the Harness remains valid.
  /// @param options Soft time and callback limits for this invocation.
  /// @return Delivery and queue statistics.
  UpdateStats update(UpdateOptions options = {});

private:
  class Impl;

  explicit Harness(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class detail::HarnessTestAccess;
};

} // namespace scry
