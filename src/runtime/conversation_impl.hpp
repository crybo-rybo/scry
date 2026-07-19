#pragma once

#include "runtime/state.hpp"

#include <memory>
#include <scry/conversation.hpp>
#include <utility>

namespace scry {

class Conversation::Impl final {
public:
  // This is an intentional sink parameter: callers transfer the prompt into the state.
  explicit Impl(
      ConversationConfig config) // NOLINT(performance-unnecessary-value-param)
      : state(std::make_shared<detail::ConversationState>()) {
    state->payload_bytes = config.system_prompt.size();
    state->config = std::move(config);
  }

  std::shared_ptr<detail::ConversationState> state{};
};

} // namespace scry
