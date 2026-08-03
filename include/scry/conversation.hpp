#pragma once

#include <cstddef>
#include <memory>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>

namespace scry {

/// Initial state for a Conversation.
struct ConversationConfig {
  /// Optional instruction prepended to the conversation history.
  std::string system_prompt{};
};

/// Move-only owner of committed model conversation history.
///
/// A Conversation may have at most one queued or active turn. History is committed
/// transactionally when Harness::update() delivers a successful terminal event; failed
/// and cancelled turns do not modify it.
class Conversation final {
public:
  /// Validates configuration and creates an empty conversation.
  /// @param config Initial conversation configuration.
  /// @return A conversation, or ErrorCategory::invalid_config on validation failure.
  [[nodiscard]] static Result<Conversation> create(ConversationConfig config = {});

  /// Restores committed history from a canonical document produced by to_json().
  /// @param json Versioned Scry conversation document.
  /// @return The restored conversation, or ErrorCategory::invalid_config if the
  /// document is malformed, unsupported, or exceeds structural constraints.
  [[nodiscard]] static Result<Conversation> from_json(Json json);

  /// Destroys this handle and its committed history when no live turn retains the
  /// state.
  ~Conversation();

  /// Moves a conversation handle without changing its history.
  Conversation(Conversation&&) noexcept;

  /// Replaces this handle with a moved conversation.
  /// @return This conversation.
  Conversation& operator=(Conversation&&) noexcept;

  /// Conversations are not copyable.
  Conversation(const Conversation&) = delete;

  /// Conversations are not copy-assignable.
  Conversation& operator=(const Conversation&) = delete;

  /// Reports whether the committed message history is empty.
  /// @return true when there are no committed messages.
  [[nodiscard]] bool empty() const noexcept;

  /// Returns the number of committed messages, excluding the system prompt.
  /// @return Committed message count.
  [[nodiscard]] std::size_t message_count() const noexcept;

  /// Serializes the last committed boundary as a canonical, versioned JSON document.
  ///
  /// Busy state and an active turn's uncommitted exchange are deliberately excluded.
  /// @return The serialized document, or an error if the current state cannot be
  /// encoded.
  [[nodiscard]] Result<Json> to_json() const;

private:
  class Impl;

  explicit Conversation(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class Harness;
};

} // namespace scry
