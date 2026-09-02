/// @file
/// @brief Private storage for the move-only Turn cancellation handle.
///
/// A Turn retains only immutable identity, the sanctioned cross-thread cancellation
/// atomic, and a weak pump route. It never owns worker state or turn callbacks.

#pragma once

#include "runtime/pump.hpp"

#include <memory>
#include <scry/turn.hpp>

namespace scry {

/// Private implementation of the public Turn handle.
///
/// The cancellation flag intentionally outlives the route when necessary, making
/// cancellation after Harness destruction harmless. The weak route supports queued-turn
/// cancellation and detach bookkeeping without prolonging Harness-owned pump state.
class Turn::Impl final {
public:
  /// Captures the identity and cancellation capabilities of an accepted route.
  ///
  /// @param active_route Accepted pump route to observe weakly.
  explicit Impl(const std::shared_ptr<detail::TurnRoute>& active_route)
      : turn_id(active_route->id()), cancelled(active_route->cancel_flag()),
        route(active_route) {}

  /// Detaches the handle without cancelling or blocking the accepted turn.
  ~Impl() {
    if (const auto active_route = route.lock()) {
      active_route->detach();
    }
  }

  /// Immutable correlation identity copied from the route.
  TurnId turn_id{};
  /// Per-turn cancellation flag shared with the worker.
  std::shared_ptr<std::atomic<bool>> cancelled{};
  /// Non-owning pump route used for cancellation commands and detach bookkeeping.
  std::weak_ptr<detail::TurnRoute> route{};
};

} // namespace scry
