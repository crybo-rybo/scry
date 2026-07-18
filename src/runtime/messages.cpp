#include "runtime/messages.hpp"

#include <type_traits>

namespace scry::detail {
namespace {

[[nodiscard]] std::size_t content_bytes(const ModelResponse& response) noexcept {
  std::size_t total = response.provider_request_id.size();
  for (const auto& block : response.content) {
    std::visit(
        [&total](const auto& value) {
          using Block = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Block, TextBlock>) {
            total += value.text.size();
          } else if constexpr (std::is_same_v<Block, ToolCallBlock>) {
            total += value.id.size() + value.name.size() + value.arguments.text.size();
          } else {
            total +=
                value.tool_call_id.size() + value.result.text.size() + sizeof(bool);
          }
        },
        block);
  }
  return total;
}

} // namespace

TurnId event_turn_id(const WorkerEvent& event) noexcept {
  return std::visit([](const auto& value) { return value.turn_id; }, event);
}

std::size_t event_payload_bytes(const WorkerEvent& event) noexcept {
  return std::visit(
      [](const auto& value) -> std::size_t {
        using Event = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Event, TextDeltaEvent>) {
          return value.text.size();
        } else if constexpr (std::is_same_v<Event, CompletionEvent>) {
          return content_bytes(value.response);
        } else if constexpr (std::is_same_v<Event, ErrorEvent>) {
          return value.error.message.size() + value.error.provider_detail.size() +
                 value.error.provider_request_id.size();
        } else {
          return 0;
        }
      },
      event);
}

} // namespace scry::detail
