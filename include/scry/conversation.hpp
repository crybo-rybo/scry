#pragma once

/**
 * @file conversation.hpp
 * @brief Transactional conversation history and persistence.
 *
 * A Conversation is application-owned state shared with accepted turns through an
 * internal route. The public handle exposes only committed history: construction,
 * canonical persistence, and lightweight inspection. Harness is the only component
 * allowed to stage or commit an exchange.
 */

#include <cstddef>
#include <memory>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>

namespace scry {

/// Initial state for a Conversation.
///
/// The aggregate is copied into Conversation-owned storage by Conversation::create().
struct ConversationConfig {
  /// Optional provider-neutral instruction prepended to every model request.
  ///
  /// The prompt is persisted by to_json() but is not counted by message_count().
  std::string system_prompt{};
};

/// Move-only owner of committed model conversation history.
///
/// A Conversation may have at most one queued or active turn. History is committed
/// transactionally when Harness::update() processes a successful terminal event;
/// failed and cancelled turns do not modify it. The complete successful exchange—user
/// message, assistant tool-call rounds, tool results, and final assistant text—appears
/// atomically.
///
/// The handle is move-only. An accepted turn retains the internal conversation route,
/// so destroying the application handle does not invalidate in-flight work. Unless a
/// turn is being accepted or completed through Harness, access is intended for the
/// host/pump thread; the type provides no general-purpose concurrent mutation API.
class Conversation final {
public:
  /// Creates an empty conversation from the supplied configuration.
  ///
  /// The returned history contains no messages; a nonempty system prompt is stored
  /// separately and is included in future provider requests.
  /// @param config Initial conversation configuration.
  /// @return The created conversation.
  [[nodiscard]] static Result<Conversation> create(ConversationConfig config = {});

  /// Restores committed history from the versioned document shape produced by
  /// to_json().
  ///
  /// Validation is strict: unknown fields, unsupported document versions, and malformed
  /// role/content combinations or embedded JSON values are rejected. Input object-key
  /// order need not already be canonical; a subsequent to_json() call canonicalizes
  /// the representation.
  /// The restored Conversation is idle; serialized data never carries busy state,
  /// callbacks, turn identifiers, or an uncommitted exchange.
  ///
  /// @warning Before Scry 1.0, persistence documents are guaranteed only within a
  /// pinned Scry version and are not a durable cross-release archive format.
  /// @param json Versioned Scry conversation document.
  /// @return The restored conversation, or ErrorCategory::invalid_config if the
  /// document is malformed, unsupported, or exceeds structural constraints.
  [[nodiscard]] static Result<Conversation> from_json(const Json& json);

  /// Destroys this handle.
  ///
  /// Committed state remains alive while an accepted turn retains its internal route;
  /// destruction never blocks, cancels work, or fires a callback.
  ~Conversation();

  /// Moves a conversation handle without changing its history.
  Conversation(Conversation&&) noexcept;

  /// Replaces this handle with a moved conversation.
  ///
  /// Any state formerly owned by this handle is released under the same nonblocking
  /// lifetime rules as destruction. The moved-from object remains destructible and
  /// assignable but otherwise has no documented operational state.
  /// @param other Conversation whose handle state is transferred.
  /// @return This conversation.
  Conversation& operator=(Conversation&& other) noexcept;

  /// Conversations are not copyable.
  Conversation(const Conversation&) = delete;

  /// Conversations are not copy-assignable.
  Conversation& operator=(const Conversation&) = delete;

  /// Reports whether the committed message history is empty.
  ///
  /// A Conversation containing only a system prompt is empty by this definition.
  /// @return true when there are no committed messages.
  [[nodiscard]] bool empty() const noexcept;

  /// Returns the number of committed messages, excluding the system prompt.
  /// @return Committed message count.
  [[nodiscard]] std::size_t message_count() const noexcept;

  /// Serializes the last committed boundary as a canonical, versioned JSON document.
  ///
  /// Busy state and an active turn's uncommitted exchange are deliberately excluded.
  /// Object keys and embedded tool JSON are emitted in Scry's canonical ordering.
  /// Scry performs no file I/O; storage, encryption, retention, and migrations remain
  /// application responsibilities.
  ///
  /// @warning Before Scry 1.0, canonical bytes and document shape may change between
  /// releases without a migration path.
  /// @return The serialized document, or an error if the current state cannot be
  /// encoded.
  [[nodiscard]] Result<Json> to_json() const;

private:
  /// Opaque conversation state shared with live pump-side turn routes.
  class Impl;

  /// Constructs the public handle around validated implementation state.
  /// @param impl Non-null implementation allocated by create() or from_json().
  explicit Conversation(std::unique_ptr<Impl> impl) noexcept;

  /// Exclusively owned handle reference to the opaque state.
  std::unique_ptr<Impl> impl_;

  friend class Harness;
};

} // namespace scry
