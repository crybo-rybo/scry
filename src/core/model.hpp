#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
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

struct ModelRequest {
  std::string model{};
  std::string system_prompt{};
  std::vector<Message> messages{};
  std::vector<ToolSchema> tools{};
  SamplingConfig sampling{};
  bool streaming{true};
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
