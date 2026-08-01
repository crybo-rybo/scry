#pragma once

#include <memory>
#include <scry/error.hpp>
#include <scry/events.hpp>
#include <scry/turn_id.hpp>

namespace scry {

/// Move-only handle for one accepted agentic exchange.
///
/// Dropping the handle detaches without cancelling or blocking: the turn continues,
/// successful history still commits, and already registered callbacks remain
/// deliverable. While attached, callbacks registered after activity begins receive
/// buffered prior events in order within the configured resource limit.
class Turn final {
public:
  /// Detaches from the turn without cancelling it.
  ~Turn();

  /// Moves attachment and cancellation control to another handle.
  Turn(Turn&&) noexcept;

  /// Detaches from the current turn and takes another handle's attachment.
  /// @return This Turn handle.
  Turn& operator=(Turn&&) noexcept;

  /// Turn handles are not copyable.
  Turn(const Turn&) = delete;

  /// Turn handles are not copy-assignable.
  Turn& operator=(const Turn&) = delete;

  /// Returns the immutable identifier assigned when the turn was accepted.
  /// @return Accepted-turn identifier.
  [[nodiscard]] TurnId id() const noexcept;

  /// Requests cooperative cancellation.
  /// @return true only when this call issued the cancellation request; false if
  /// cancellation was already requested, the turn was terminal, or the handle is moved
  /// from.
  [[nodiscard]] bool cancel() noexcept;

  /// Registers the streamed-text observer.
  /// @param callback Move-only callback invoked inside Harness::update().
  /// @return Success, or ErrorCategory::invalid_state if registration is unavailable.
  [[nodiscard]] Status on_text_delta(TextDeltaCallback callback);

  /// Registers the tool-call observer.
  ///
  /// Tool execution does not require this observer. It runs on the update() thread
  /// after the canonical result is accepted.
  /// @param callback Move-only callback invoked inside Harness::update().
  /// @return Success, or ErrorCategory::invalid_state if registration is unavailable.
  [[nodiscard]] Status on_tool_call(ToolCallCallback callback);

  /// Registers the successful terminal callback.
  /// @param callback Move-only callback invoked inside Harness::update().
  /// @return Success, or ErrorCategory::invalid_state if registration is unavailable.
  [[nodiscard]] Status on_completion(CompletionCallback callback);

  /// Registers the failed terminal callback.
  /// @param callback Move-only callback invoked inside Harness::update().
  /// @return Success, or ErrorCategory::invalid_state if registration is unavailable.
  [[nodiscard]] Status on_error(ErrorCallback callback);

  /// Registers the cancelled terminal callback.
  /// @param callback Move-only callback invoked inside Harness::update().
  /// @return Success, or ErrorCategory::invalid_state if registration is unavailable.
  [[nodiscard]] Status on_cancelled(CancelledCallback callback);

private:
  class Impl;

  explicit Turn(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class Harness;
};

} // namespace scry
