#include "runtime/state.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scry {

Conversation::Conversation(std::shared_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Conversation::~Conversation() = default;
Conversation::Conversation(Conversation&&) noexcept = default;
Conversation& Conversation::operator=(Conversation&&) noexcept = default;

// No ConversationConfig is rejected today; see the create() contract in
// include/scry/conversation.hpp before adding a check here.
Result<Conversation> Conversation::create(ConversationConfig config) {
  auto impl = std::make_shared<Impl>();
  impl->payload_bytes = config.system_prompt.size();
  impl->config = std::move(config);
  return Conversation{std::move(impl)};
}

bool Conversation::empty() const noexcept {
  return impl_ == nullptr || impl_->messages->empty();
}

std::size_t Conversation::message_count() const noexcept {
  return impl_ == nullptr ? 0 : impl_->messages->size();
}

const std::vector<Message>& Conversation::messages() const noexcept {
  static const std::vector<Message> none{};
  return impl_ == nullptr ? none : *impl_->messages;
}

const std::string& Conversation::system_prompt() const noexcept {
  static const std::string none{};
  return impl_ == nullptr ? none : impl_->config.system_prompt;
}

bool Conversation::busy() const noexcept { return impl_ != nullptr && impl_->busy; }

} // namespace scry
