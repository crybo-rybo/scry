#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
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

using SharedMessage = std::shared_ptr<const Message>;

// Immutable conversation history is shared across send() snapshots, tool-round
// resends, and the pump commit. The converting constructors keep tests and
// designated initializers writing ordinary Message values.
struct SharedHistory : std::vector<SharedMessage> {
  using std::vector<SharedMessage>::vector;
  using std::vector<SharedMessage>::operator=;

  SharedHistory() = default;

  SharedHistory(std::initializer_list<Message> messages) {
    reserve(messages.size());
    for (const auto& message : messages) {
      emplace_back(std::make_shared<const Message>(message));
    }
  }

  SharedHistory& operator=(std::initializer_list<Message> messages) {
    SharedHistory assigned{messages};
    vector::operator=(std::move(assigned));
    return *this;
  }
};

[[nodiscard]] inline SharedMessage share_message(Message message) {
  return std::make_shared<const Message>(std::move(message));
}

template <typename Fn>
void mutate_shared_message(SharedHistory& history, const std::size_t index, Fn&& fn) {
  Message copy = *history[index];
  static_cast<Fn&&>(fn)(copy);
  history[index] = share_message(std::move(copy));
}

struct ToolSchema {
  std::string name{};
  std::string description{};
  Json input_schema{};
};

struct ModelRequest {
  std::string system_prompt{};
  SharedHistory messages{};
  std::shared_ptr<const std::vector<ToolSchema>> tools{};
  SamplingConfig sampling{};
};

[[nodiscard]] inline std::shared_ptr<const std::vector<ToolSchema>>
share_tool_schemas(std::vector<ToolSchema> schemas) {
  return std::make_shared<const std::vector<ToolSchema>>(std::move(schemas));
}

[[nodiscard]] inline const std::vector<ToolSchema>&
tool_schemas(const ModelRequest& request) noexcept {
  static const std::vector<ToolSchema> empty{};
  return request.tools ? *request.tools : empty;
}

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

[[nodiscard]] inline std::size_t
message_payload_bytes(const SharedMessage& message) noexcept {
  return message ? message_payload_bytes(*message) : 0;
}

} // namespace scry::detail
