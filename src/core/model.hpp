/// @file
/// @brief Provider-neutral messages, requests, responses, and payload accounting.
///
/// These values form the internal contract between the runtime, turn machine,
/// and provider adapters. They deliberately contain no provider wire-format or
/// transport types, keeping dialect-specific JSON below the adapter seam.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <scry/config.hpp>
#include <scry/events.hpp>
#include <scry/json.hpp>
#include <string>
#include <variant>
#include <vector>

namespace scry::detail {

/// @brief Speaker role for a provider-neutral conversation message.
enum class Role : std::uint8_t {
  user,      ///< Input supplied by the host or a tool result returned to the model.
  assistant, ///< Model-produced text and tool calls.
};

/// @brief A contiguous piece of natural-language message content.
struct TextBlock {
  std::string text{}; ///< Owning UTF-8 text bytes.
};

/// @brief Provider-neutral request by the model to invoke one named tool.
///
/// The provider adapter supplies a stable call identifier and validated JSON-object
/// arguments. TurnMachine canonicalizes the arguments before publishing the call for
/// app-thread dispatch. The identifier is the turn-local at-most-once dispatch key.
struct ToolCallBlock {
  std::string id{};   ///< Stable provider call identifier; nonempty after validation.
  std::string name{}; ///< Provider-requested tool name; registration is checked later.
  Json arguments{};   ///< Validated object; canonical after TurnMachine validation.
};

/// @brief Provider-neutral result associated with an earlier tool call.
struct ToolResultBlock {
  std::string tool_call_id{}; ///< Identifier copied from the matching call.
  Json result{};              ///< Canonical JSON returned to the model.
  bool is_error{false};       ///< Whether `result` is a model-visible tool error.
};

/// @brief Closed set of content forms carried by neutral messages.
using ContentBlock = std::variant<TextBlock, ToolCallBlock, ToolResultBlock>;

/// @brief One provider-neutral conversation message.
///
/// Provider adapters are responsible for expanding this representation into
/// dialect-specific wire messages while preserving block order.
struct Message {
  Role role{Role::user};               ///< Author of the message.
  std::vector<ContentBlock> content{}; ///< Ordered content within the message.
};

/// @brief Serializable provider-neutral description of one registered tool.
struct ToolSchema {
  std::string name{};        ///< Unique registry name exposed to the model.
  std::string description{}; ///< Human-readable purpose exposed to the model.
  Json input_schema{};       ///< Canonical JSON-object input schema.
};

/// @brief Immutable committed-history collection shared across a worker boundary.
///
/// Ownership is collection-level, so taking a request snapshot increments one
/// control block rather than copying messages or allocating per element. The
/// Conversation reseats its block copy-on-write before later mutation.
using HistorySnapshot = std::shared_ptr<const std::vector<Message>>;

/// @brief Immutable tool-schema collection shared by accepted turns.
///
/// Only neutral schemas cross to the worker. Pump-owned handlers are stored in
/// a separate snapshot and never cross the thread boundary.
using SchemaSnapshot = std::shared_ptr<const std::vector<ToolSchema>>;

/// @brief Complete neutral input for one model request attempt.
///
/// `history` is a shared immutable prefix committed before this turn;
/// `messages` is the machine-owned suffix for the active exchange. A request
/// snapshot handed to an attempt never observes later tool-round appends.
struct ModelRequest {
  std::string system_prompt{};     ///< Host-supplied system instruction.
  HistorySnapshot history{};       ///< Immutable committed Conversation prefix.
  std::vector<Message> messages{}; ///< Private messages introduced by this turn.
  SchemaSnapshot tools{};          ///< Immutable schemas captured at acceptance.
  SamplingConfig sampling{};       ///< Already-validated provider sampling values.

  /// @brief Counts messages in the shared prefix and private suffix.
  /// @return Total logical message count without traversing either collection.
  [[nodiscard]] std::size_t message_count() const noexcept {
    return (history ? history->size() : 0U) + messages.size();
  }
};

using ::scry::FinishReason; ///< Public provider-neutral completion reason.
using ::scry::Usage;        ///< Public aggregate token accounting.

/// @brief Fully decoded neutral result of one successful model request.
///
/// A response may request one or more tools or carry the final assistant
/// answer. Provider adapters accumulate streaming fragments into this value.
struct ModelResponse {
  std::vector<ContentBlock> content{}; ///< Ordered assistant content blocks.
  FinishReason finish_reason{FinishReason::unknown}; ///< Provider-neutral stop cause.
  Usage usage{}; ///< Token counts reported for this request, if available.
  std::string provider_request_id{}; ///< Provider correlation identifier, if any.
};

/// @brief Adds two payload sizes without unsigned wraparound.
/// @param left Existing byte count.
/// @param right Additional byte count.
/// @return The exact sum or `size_t` maximum when it would overflow.
[[nodiscard]] inline std::size_t
saturating_payload_add(const std::size_t left, const std::size_t right) noexcept {
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  return right > maximum - left ? maximum : left + right;
}

/// @brief Counts payload bytes owned by a text block.
/// @param block Block to inspect.
/// @return Number of text bytes, excluding allocator/object overhead.
[[nodiscard]] inline std::size_t
content_payload_bytes(const TextBlock& block) noexcept {
  return block.text.size();
}

/// @brief Counts payload bytes owned by a tool-call block.
/// @param block Block to inspect.
/// @return Saturating sum of identifier, name, and JSON argument bytes.
[[nodiscard]] inline std::size_t
content_payload_bytes(const ToolCallBlock& block) noexcept {
  return saturating_payload_add(
      saturating_payload_add(block.id.size(), block.name.size()),
      block.arguments.text.size());
}

/// @brief Counts payload bytes owned by a tool-result block.
/// @param block Block to inspect.
/// @return Saturating sum of identifier, JSON result, and error-flag bytes.
[[nodiscard]] inline std::size_t
content_payload_bytes(const ToolResultBlock& block) noexcept {
  return saturating_payload_add(
      saturating_payload_add(block.tool_call_id.size(), block.result.text.size()),
      sizeof(bool));
}

/// @brief Counts payload bytes owned by any neutral content block.
/// @param block Block to visit.
/// @return The matching concrete block's saturating payload count.
[[nodiscard]] inline std::size_t
content_payload_bytes(const ContentBlock& block) noexcept {
  return std::visit([](const auto& value) { return content_payload_bytes(value); },
                    block);
}

/// @brief Counts the configured payload bytes in one neutral message.
/// @param message Message to inspect.
/// @return Saturating sum across its ordered content blocks.
[[nodiscard]] inline std::size_t
message_payload_bytes(const Message& message) noexcept {
  std::size_t total = 0;
  for (const auto& block : message.content) {
    total = saturating_payload_add(total, content_payload_bytes(block));
  }
  return total;
}

} // namespace scry::detail
