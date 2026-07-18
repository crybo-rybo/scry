#include "runtime/state.hpp"

#include <type_traits>

namespace scry::detail {

std::size_t message_payload_bytes(const Message& message) noexcept {
  std::size_t total = 0;
  for (const auto& block : message.content) {
    std::visit(
        [&total](const auto& value) {
          using Block = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Block, TextBlock>) {
            total += value.text.size();
          } else if constexpr (std::is_same_v<Block, ToolCallBlock>) {
            total += value.id.size() + value.name.size() + value.arguments.text.size();
          } else {
            total += value.tool_call_id.size() + value.result.text.size();
          }
        },
        block);
  }
  return total;
}

std::string response_text(const ModelResponse& response) {
  std::string text;
  for (const auto& block : response.content) {
    if (const auto* content = std::get_if<TextBlock>(&block)) {
      text += content->text;
    }
  }
  return text;
}

} // namespace scry::detail
