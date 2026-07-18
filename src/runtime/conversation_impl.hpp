#pragma once

#include "runtime/state.hpp"

#include <memory>
#include <scry/conversation.hpp>
#include <utility>

namespace scry {

class Conversation::Impl final {
public:
  explicit Impl(ConversationConfig config)
      : state(std::make_shared<detail::ConversationState>(
            detail::ConversationState{.config = std::move(config)})) {}

  std::shared_ptr<detail::ConversationState> state{};
};

} // namespace scry
