#include "runtime/conversation_impl.hpp"

#include <utility>

namespace scry {

Conversation::Conversation(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Conversation::~Conversation() = default;
Conversation::Conversation(Conversation&&) noexcept = default;
Conversation& Conversation::operator=(Conversation&&) noexcept = default;

Result<Conversation> Conversation::create(ConversationConfig config) {
  return Conversation{std::make_unique<Impl>(std::move(config))};
}

bool Conversation::empty() const noexcept {
  return impl_ == nullptr || impl_->state->messages.empty();
}

std::size_t Conversation::message_count() const noexcept {
  return impl_ == nullptr ? 0 : impl_->state->messages.size();
}

} // namespace scry
