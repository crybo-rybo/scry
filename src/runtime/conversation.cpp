#include "runtime/conversation_impl.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scry {

Conversation::Conversation(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Conversation::~Conversation() = default;
Conversation::Conversation(Conversation&&) noexcept = default;
Conversation& Conversation::operator=(Conversation&&) noexcept = default;

// No ConversationConfig is rejected today; see the create() contract in
// include/scry/conversation.hpp before adding a check here.
Result<Conversation> Conversation::create(ConversationConfig config) {
  return Conversation{std::make_unique<Impl>(std::move(config))};
}

bool Conversation::empty() const noexcept {
  return impl_ == nullptr || impl_->state->messages->empty();
}

std::size_t Conversation::message_count() const noexcept {
  return impl_ == nullptr ? 0 : impl_->state->messages->size();
}

const std::vector<Message>& Conversation::messages() const noexcept {
  static const std::vector<Message> none{};
  return impl_ == nullptr ? none : *impl_->state->messages;
}

const std::string& Conversation::system_prompt() const noexcept {
  static const std::string none{};
  return impl_ == nullptr ? none : impl_->state->config.system_prompt;
}

bool Conversation::busy() const noexcept {
  return impl_ != nullptr && impl_->state->busy;
}

} // namespace scry
