#pragma once

/**
 * @file harness.hpp
 * @brief Host-driven asynchronous runtime and synchronous convenience entry point.
 *
 * Harness is Scry's configured owner: it joins provider/auth configuration, the
 * additive ToolRegistry, one worker actor, message queues, and the app-thread pump.
 * It deliberately does not own an event loop; the host advances callback and tool
 * delivery by calling Harness::update().
 */

#include <memory>
#include <scry/config.hpp>
#include <scry/conversation.hpp>
#include <scry/error.hpp>
#include <scry/events.hpp>
#include <scry/tool_registry.hpp>
#include <scry/turn.hpp>
#include <string>

namespace scry {

namespace detail {
/// Test-only friend shim for constructing a Harness with injected internal seams.
class HarnessTestAccess;
} // namespace detail

/// Configured runtime that owns tools, network state, worker I/O, and callback
/// delivery.
///
/// A Harness never owns the host's main loop. Blocking network work progresses on its
/// worker while the host periodically calls update() to execute tool handlers and
/// deliver application-facing callbacks. Exactly one transfer is active at a time;
/// accepted turns wait in FIFO order up to Config::limits.max_pending_turns.
///
/// All mutable provider/runtime state is instance-local, so independently configured
/// Harnesses can coexist. The handle is move-only and is not itself a general-purpose
/// concurrent API: call send(), tools(), and update() according to the host thread's
/// ownership discipline. Turn::cancel() is the separately documented thread-safe
/// cancellation operation.
class Harness final {
public:
  /// Validates configuration and starts a Harness-owned worker.
  ///
  /// Validation, provider selection, curl capability checks, and worker startup happen
  /// before success is returned. No partially active Harness escapes on failure.
  /// @param config Provider, retry, timeout, and resource configuration.
  /// @return A Harness; ErrorCategory::invalid_config for invalid values or missing
  /// runtime capabilities, ErrorCategory::network if process-wide libcurl
  /// initialization fails, or ErrorCategory::resource_limit if the worker thread
  /// cannot be started.
  [[nodiscard]] static Result<Harness> create(Config config);

  /// Cancels outstanding work, stops Scry-owned I/O, and joins the worker.
  ///
  /// Destruction requests cancellation for every turn, aborts Scry-owned transfers,
  /// joins the worker, and discards undelivered callbacks. No tool handler or callback
  /// begins after shutdown starts. The configured transport/shutdown bounds govern
  /// Scry-owned waits; application callbacks never run on the worker.
  ~Harness();

  /// Moves ownership of the worker and all Harness state.
  Harness(Harness&&) noexcept;

  /// Replaces this Harness with another moved Harness.
  ///
  /// Releasing an active destination performs the same shutdown and worker join as its
  /// destructor before ownership is replaced. The source remains destructible and
  /// assignable but has no active runtime.
  /// @param other Harness whose runtime ownership is transferred.
  /// @return This Harness.
  Harness& operator=(Harness&& other) noexcept;

  /// Harnesses are not copyable.
  Harness(const Harness&) = delete;

  /// Harnesses are not copy-assignable.
  Harness& operator=(const Harness&) = delete;

  /// Returns the Harness-owned additive tool registry.
  ///
  /// The reference remains valid until this Harness is destroyed, moved from, or
  /// move-assigned. The registry cannot be moved out of its owner.
  /// @pre This Harness has not been moved from.
  /// @return Mutable registry for future turns.
  [[nodiscard]] ToolRegistry& tools() noexcept;

  /// Returns the Harness-owned additive tool registry.
  ///
  /// The reference remains valid until this Harness is destroyed, moved from, or
  /// move-assigned.
  /// @pre This Harness has not been moved from.
  /// @return Read-only registry view.
  [[nodiscard]] const ToolRegistry& tools() const noexcept;

  /// Accepts an asynchronous user turn.
  ///
  /// The call validates admission synchronously, snapshots current tool registrations,
  /// and returns without waiting for network I/O. The host must call update() to
  /// execute app-thread tools, process terminal commit/rollback, and deliver callbacks.
  ///
  /// Callbacks are attached infallibly and atomically as the turn is accepted, so no
  /// event can precede them and there is no later registration or replay step. When
  /// TurnCallbacks::on_finished is non-empty, an accepted turn invokes it exactly once
  /// unless this Harness is destroyed first; see ~Harness().
  /// @param conversation Conversation that receives the exchange on successful
  /// completion.
  /// @param user_message User text appended transactionally if the turn succeeds.
  /// @param callbacks Optional per-turn observers delivered inside update().
  /// @return A controllable Turn handle, or an immediate invalid-state, busy, or
  /// resource-limit admission error. A failed admission leaves the Conversation and
  /// registry snapshot generation unchanged.
  [[nodiscard]] Result<Turn> send(Conversation& conversation, std::string user_message,
                                  TurnCallbacks callbacks = {});

  /// Runs one turn synchronously on top of send() and update().
  ///
  /// This is the only public operation that waits for network I/O. It is intended for
  /// command line programs and tests rather than host-owned main loops. It delegates to
  /// send() and repeatedly pumps update(), so it preserves the same transactional,
  /// tool-dispatch, retry, and callback-thread semantics rather than creating a second
  /// execution path. It cannot be called from inside an update() callback.
  /// @param conversation Conversation that receives the exchange on success.
  /// @param user_message User text sent to the configured model.
  /// @return The successful completion, an immediate admission error, or the accepted
  /// turn's terminal error.
  [[nodiscard]] Result<Completion> send_and_wait(Conversation& conversation,
                                                 std::string user_message);

  /// Pumps queued events, app-thread tool handlers, and callbacks on the calling
  /// thread.
  ///
  /// Reentrant calls are rejected and report UpdateStats::budget_exhausted. Exceptions
  /// escaping an application callback propagate synchronously after the event counts as
  /// delivered; the Harness remains valid. Limits are soft scheduling boundaries:
  /// callbacks and handlers already running are never preempted.
  /// @param options Soft time and callback limits for this invocation.
  /// @return Delivery and queue statistics.
  UpdateStats update(UpdateOptions options = {});

private:
  /// Opaque runtime implementation owning the worker, queues, pump, and registry.
  class Impl;

  /// Constructs a public handle around a fully initialized runtime.
  /// @param impl Non-null implementation produced by create() or test access.
  explicit Harness(std::unique_ptr<Impl> impl) noexcept;

  /// Exclusive ownership of the opaque runtime implementation.
  std::unique_ptr<Impl> impl_;

  friend class detail::HarnessTestAccess;
};

} // namespace scry
