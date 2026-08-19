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

enum class Role : std::uint8_t {
  user,
  assistant,
};

struct TextBlock {
  std::string text{};
};

struct ToolCallBlock {
  std::string id{};
  std::string name{};
  Json arguments{};
};

struct ToolResultBlock {
  std::string tool_call_id{};
  Json result{};
  bool is_error{false};
};

using ContentBlock = std::variant<TextBlock, ToolCallBlock, ToolResultBlock>;

struct Message {
  Role role{Role::user};
  std::vector<ContentBlock> content{};
};

struct ToolSchema {
  std::string name{};
  std::string description{};
  Json input_schema{};
};

// Immutable collections shared across thread and turn boundaries. Ownership is
// collection-level: a snapshot is one control block rather than one per
// message or schema, so sharing costs an atomic increment instead of a deep
// copy, and readers retain locality of the underlying vector.
using HistorySnapshot = std::shared_ptr<const std::vector<Message>>;
using SchemaSnapshot = std::shared_ptr<const std::vector<ToolSchema>>;

struct ModelRequest {
  std::string system_prompt{};
  // Committed history shared with the owning Conversation at send time. It is
  // immutable for the request's lifetime; the Conversation reseats its own
  // block copy-on-write before a commit if any request still references it.
  HistorySnapshot history{};
  // Messages introduced by this turn: the user message and each tool round.
  // The turn machine owns this suffix privately and reseats it copy-on-write.
  std::vector<Message> messages{};
  SchemaSnapshot tools{};
  SamplingConfig sampling{};

  [[nodiscard]] std::size_t message_count() const noexcept {
    return (history ? history->size() : 0U) + messages.size();
  }
};

using ::scry::FinishReason;
using ::scry::Usage;

struct ModelResponse {
  std::vector<ContentBlock> content{};
  FinishReason finish_reason{FinishReason::unknown};
  Usage usage{};
  std::string provider_request_id{};
};

[[nodiscard]] inline std::size_t
saturating_payload_add(const std::size_t left, const std::size_t right) noexcept {
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  return right > maximum - left ? maximum : left + right;
}

[[nodiscard]] inline std::size_t
content_payload_bytes(const TextBlock& block) noexcept {
  return block.text.size();
}

[[nodiscard]] inline std::size_t
content_payload_bytes(const ToolCallBlock& block) noexcept {
  return saturating_payload_add(
      saturating_payload_add(block.id.size(), block.name.size()),
      block.arguments.text.size());
}

[[nodiscard]] inline std::size_t
content_payload_bytes(const ToolResultBlock& block) noexcept {
  return saturating_payload_add(
      saturating_payload_add(block.tool_call_id.size(), block.result.text.size()),
      sizeof(bool));
}

[[nodiscard]] inline std::size_t
content_payload_bytes(const ContentBlock& block) noexcept {
  return std::visit([](const auto& value) { return content_payload_bytes(value); },
                    block);
}

[[nodiscard]] inline std::size_t
message_payload_bytes(const Message& message) noexcept {
  std::size_t total = 0;
  for (const auto& block : message.content) {
    total = saturating_payload_add(total, content_payload_bytes(block));
  }
  return total;
}

} // namespace scry::detail
