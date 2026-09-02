/// @file
/// @brief Private storage for the Conversation PImpl handle.
///
/// The implementation owns the pump-side ConversationState shared by the public handle
/// and any accepted turn routes. Provider requests receive only immutable history
/// snapshots, never this implementation object.

#pragma once

#include "runtime/state.hpp"

#include <memory>
#include <scry/conversation.hpp>
#include <utility>

namespace scry {

/// Pump-side implementation of the public Conversation handle.
///
/// The implementation is deliberately small: all live conversation data resides in one
/// shared ConversationState so an accepted turn can retain the state even when the
/// original handle moves. Committed history changes only during successful terminal
/// processing in the pump; admission and terminalization also update the busy flag.
class Conversation::Impl final {
public:
  /// Creates empty committed history from an application configuration.
  ///
  /// @param config Initial configuration and system prompt.
  // This is an intentional sink parameter: callers transfer the prompt into the state.
  explicit Impl(
      ConversationConfig config) // NOLINT(performance-unnecessary-value-param)
      : state(std::make_shared<detail::ConversationState>()) {
    state->payload_bytes = config.system_prompt.size();
    state->config = std::move(config);
  }

  /// Shared pump-side state retained by the handle and any live turn route.
  std::shared_ptr<detail::ConversationState> state{};
};

} // namespace scry
