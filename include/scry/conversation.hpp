#pragma once

#include <cstddef>
#include <memory>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/message.hpp>
#include <string>
#include <vector>

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
  /// Creates an empty conversation.
  ///
  /// Every ConversationConfig is currently accepted; the factory returns Result so a
  /// future constraint can be reported as ErrorCategory::invalid_config without a
  /// source break (API-010).
  /// @param config Initial conversation configuration.
  /// @return A conversation. No configuration is rejected today.
  [[nodiscard]] static Result<Conversation> create(ConversationConfig config = {});

  /// Restores committed history from a canonical document produced by to_json().
  /// @param json Versioned Scry conversation document.
  /// @return The restored conversation, or ErrorCategory::invalid_config if the
  /// document is malformed, unsupported, or exceeds structural constraints.
  [[nodiscard]] static Result<Conversation> from_json(const Json& json);

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

  /// Returns the committed message history, oldest first, excluding the system
  /// prompt.
  ///
  /// The reference is borrowed: it stays valid until the next Harness::update() that
  /// commits a turn into this Conversation, or until this handle is moved or
  /// destroyed. Copy the messages to retain them. A moved-from handle returns an
  /// empty history.
  /// @return Committed messages in commit order.
  [[nodiscard]] const std::vector<Message>& messages() const noexcept;

  /// Returns the system prompt supplied at creation or restored by from_json().
  /// @return The system prompt, empty when none was supplied.
  [[nodiscard]] const std::string& system_prompt() const noexcept;

  /// Reports whether a turn is queued or in flight on this Conversation.
  ///
  /// This is exactly the condition under which Harness::send() reports
  /// ErrorCategory::busy. A moved-from handle is never busy.
  /// @return true while a turn owns this Conversation.
  [[nodiscard]] bool busy() const noexcept;

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
