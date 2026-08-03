#pragma once

#include <memory>
#include <scry/turn_id.hpp>

namespace scry {

/// Move-only cancellation handle for one accepted agentic exchange.
///
/// Callbacks belong to the turn itself and are supplied to Harness::send(), so this
/// handle only identifies and cancels. Dropping it detaches without cancelling or
/// blocking: the turn continues, successful history still commits, and the
/// TurnCallbacks supplied at send remain deliverable.
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
  ///
  /// The turn still terminates through TurnCallbacks::on_finished, with an Error whose
  /// category is ErrorCategory::cancelled.
  /// @return true only when this call issued the cancellation request; false if
  /// cancellation was already requested, the turn was terminal, or the handle is moved
  /// from.
  bool cancel() noexcept;

private:
  class Impl;

  explicit Turn(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class Harness;
};

} // namespace scry
