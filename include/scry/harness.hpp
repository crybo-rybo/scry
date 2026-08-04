#pragma once

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
  /// @param conversation Conversation that receives the exchange on successful
  /// completion.
  /// @param user_message User text appended transactionally if the turn succeeds.
  /// @return A controllable Turn handle, or an immediate admission error.
  [[nodiscard]] Result<Turn> send(Conversation& conversation, std::string user_message);

  /// Runs one turn synchronously on top of send() and update().
  ///
  /// This is the only public operation that waits for network I/O. It is intended for
  /// command line programs and tests rather than host-owned main loops.
  /// @param conversation Conversation that receives the exchange on success.
  /// @param user_message User text sent to the configured model.
  /// @return The successful completion or terminal error.
  [[nodiscard]] Result<Completion> send_and_wait(Conversation& conversation,
                                                 std::string user_message);

  /// Pumps queued events, app-thread tool handlers, and callbacks on the calling
  /// thread.
  ///
  /// Reentrant calls are rejected and reported through UpdateStats. Exceptions escaping
  /// an application callback propagate synchronously after the event counts as
  /// delivered; the Harness remains valid.
  /// @param options Soft time and callback limits for this invocation.
  /// @return Delivery and queue statistics.
  UpdateStats update(UpdateOptions options = {});

private:
  class Impl;

  [[nodiscard]] static std::unique_ptr<ToolRegistry> make_tool_registry();

  explicit Harness(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class detail::HarnessTestAccess;
};

} // namespace scry
