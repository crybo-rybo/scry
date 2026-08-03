#include "runtime/messages.hpp"

#include "runtime/state.hpp"

#include <type_traits>

namespace scry::detail {
namespace {

[[nodiscard]] std::size_t
exchange_bytes(const std::vector<Message>& exchange) noexcept {
  std::size_t total = 0;
  for (const auto& message : exchange) {
    total = saturating_payload_add(total, message_payload_bytes(message));
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
        } else if constexpr (std::is_same_v<Event, ToolCallEvent>) {
          return content_payload_bytes(value.call);
        } else if constexpr (std::is_same_v<Event, CompletionEvent>) {
          return saturating_payload_add(value.provider_request_id.size(),
                                        exchange_bytes(value.exchange));
        } else if constexpr (std::is_same_v<Event, ErrorEvent>) {
          return saturating_payload_add(
              saturating_payload_add(value.error.message.size(),
                                     value.error.provider_detail.size()),
              value.error.provider_request_id.size());
        } else {
          return 0;
        }
      },
      event);
}

} // namespace scry::detail
