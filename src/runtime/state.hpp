#pragma once

#include "core/model.hpp"

#include <cstddef>
#include <memory>
#include <scry/conversation.hpp>
#include <vector>

namespace scry::detail {

struct ConversationState {
  ConversationConfig config{};
  // Committed history, held in one shared block. send() hands the request a
  // const view of it instead of copying every message, so the pump copies the
  // block before appending whenever an in-flight request still shares it.
  std::shared_ptr<std::vector<Message>> messages{
      std::make_shared<std::vector<Message>>()};
  // The system prompt plus every committed message.
  std::size_t payload_bytes{};
  bool busy{false};
};

} // namespace scry::detail

namespace scry {

// The public handle's state is the shared state itself, so a route holds the
// same block the handle does and every accessor is one hop away.
class Conversation::Impl final : public detail::ConversationState {};

} // namespace scry
