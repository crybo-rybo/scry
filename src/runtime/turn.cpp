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

bool Turn::finished() const noexcept {
  if (impl_ == nullptr) {
    return true;
  }
  const auto route = impl_->route.lock();
  return route == nullptr || route->finished();
}

bool Turn::cancel() noexcept {
  if (impl_ == nullptr) {
    return false;
  }
  if (const auto route = impl_->route.lock()) {
    return route->cancel();
  }
  return false;
}

bool Turn::disconnect() noexcept {
  if (impl_ == nullptr) {
    return false;
  }
  if (const auto route = impl_->route.lock()) {
    return route->disconnect();
  }
  return false;
}

} // namespace scry
