/// @file
/// @brief Implements the detached, move-only Turn identity and cancellation handle.
///
/// A handle never owns worker execution or callbacks. Its strong atomic and weak pump
/// route make repeated cancellation and cancellation after Harness teardown harmless.

#include "runtime/turn_impl.hpp"

#include <utility>

namespace scry {

Turn::Turn(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Turn::~Turn() = default;
Turn::Turn(Turn&&) noexcept = default;
Turn& Turn::operator=(Turn&&) noexcept = default;

TurnId Turn::id() const noexcept {
  return impl_ == nullptr ? TurnId{} : impl_->turn_id;
}

bool Turn::cancel() noexcept {
  if (impl_ == nullptr || impl_->cancelled == nullptr) {
    return false;
  }
  if (const auto route = impl_->route.lock()) {
    return route->cancel();
  }
  return !impl_->cancelled->exchange(true, std::memory_order_relaxed);
}

} // namespace scry
