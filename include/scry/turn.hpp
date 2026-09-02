#pragma once

/**
 * @file turn.hpp
 * @brief Lightweight identity and cooperative-cancellation handle for accepted work.
 *
 * A Turn deliberately does not own callbacks, worker state, or completion. Those live
 * in the Harness route installed during send(); the handle retains only the
 * capabilities needed to identify and cancel the accepted exchange.
 */

#include <memory>
#include <scry/turn_id.hpp>

namespace scry {

/// Move-only cancellation handle for one accepted agentic exchange.
///
/// Callbacks belong to the accepted exchange and are supplied to Harness::send(), so
/// this handle only identifies and cancels. Dropping it detaches without cancelling or
/// blocking: the turn continues, successful history still commits, and the
/// TurnCallbacks supplied at send remain deliverable.
///
/// cancel() is safe to call from the application thread at any point, including after
/// terminal delivery or Harness destruction. The handle is move-only so there is one
/// explicit owner of its cancellation capability; moving does not affect the turn.
class Turn final {
public:
  /// Detaches from the turn without cancelling it.
  ///
  /// The Harness-owned route, callback set, and Conversation commit lifecycle continue
  /// independently. Destruction never waits for network or application work.
  ~Turn();

  /// Moves attachment and cancellation control to another handle.
  Turn(Turn&&) noexcept;

  /// Detaches from the current turn and takes another handle's attachment.
  ///
  /// Releasing the current attachment does not cancel its turn. The source becomes a
  /// moved-from handle whose id() is invalid and whose cancel() returns false.
  /// @param other Turn whose identity and cancellation capability are transferred.
  /// @return This Turn handle.
  Turn& operator=(Turn&& other) noexcept;

  /// Turn handles are not copyable.
  Turn(const Turn&) = delete;

  /// Turn handles are not copy-assignable.
  Turn& operator=(const Turn&) = delete;

  /// Returns the immutable identifier assigned when the turn was accepted.
  /// @return Accepted-turn identifier, or an invalid zero TurnId for a moved-from
  /// handle.
  [[nodiscard]] TurnId id() const noexcept;

  /// Requests cooperative cancellation.
  ///
  /// The request sets the per-turn cancellation flag and, while the Harness route is
  /// alive, also covers removal from its FIFO queue before I/O. A running application
  /// handler is never preempted; cancellation takes effect after that handler returns,
  /// suppressing its result and any remaining calls in the provider batch.
  ///
  /// When a non-empty TurnCallbacks::on_finished was supplied to Harness::send(), the
  /// turn still terminates through it with an Error whose category is
  /// ErrorCategory::cancelled, unless Harness destruction begins first.
  /// @return true when this call first sets the handle's cancellation flag. A live
  /// terminal route, a previously cancelled turn, and a moved-from handle return false.
  /// After Harness destruction, the first call may return true even though no work
  /// remains; it records the detached handle's cancellation state only.
  bool cancel() noexcept;

private:
  /// Opaque identity, cancellation flag, and weak pump-route reference.
  class Impl;

  /// Constructs a handle for an already accepted turn route.
  /// @param impl Non-null implementation created by Harness::send().
  explicit Turn(std::unique_ptr<Impl> impl) noexcept;

  /// Exclusive ownership of this public attachment; not ownership of turn execution.
  std::unique_ptr<Impl> impl_;

  friend class Harness;
};

} // namespace scry
