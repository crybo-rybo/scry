#include "runtime/turn_impl.hpp"

#include <utility>

namespace scry {
namespace {

[[nodiscard]] Status inactive_turn() {
  return std::unexpected(Error{
      .category = ErrorCategory::invalid_state,
      .message = "Turn is no longer attached to an active Harness",
  });
}

} // namespace

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

Status Turn::on_text_delta(TextDeltaCallback callback) {
  const auto route = impl_ == nullptr ? nullptr : impl_->route.lock();
  return route ? route->register_text(std::move(callback)) : inactive_turn();
}

Status Turn::on_tool_call(ToolCallCallback callback) {
  const auto route = impl_ == nullptr ? nullptr : impl_->route.lock();
  return route ? route->register_tool(std::move(callback)) : inactive_turn();
}

Status Turn::on_complete(CompletionCallback callback) {
  const auto route = impl_ == nullptr ? nullptr : impl_->route.lock();
  return route ? route->register_completion(std::move(callback)) : inactive_turn();
}

Status Turn::on_error(ErrorCallback callback) {
  const auto route = impl_ == nullptr ? nullptr : impl_->route.lock();
  return route ? route->register_error(std::move(callback)) : inactive_turn();
}

Status Turn::on_cancelled(CancelledCallback callback) {
  const auto route = impl_ == nullptr ? nullptr : impl_->route.lock();
  return route ? route->register_cancelled(std::move(callback)) : inactive_turn();
}

} // namespace scry
