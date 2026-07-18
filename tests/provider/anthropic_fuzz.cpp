#include "core/provider.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  const auto bytes = std::span{data, size};
  if (bytes.empty()) {
    return 0;
  }
  const auto input = std::string_view{reinterpret_cast<const char*>(bytes.data() + 1),
                                      bytes.size() - 1};
  auto adapter = scry::detail::make_provider_adapter(scry::ProviderDialect::anthropic);
  if (!adapter) {
    return 0;
  }

  if ((bytes.front() & 1U) == 0U) {
    const scry::detail::TransportResult result{.status_code = 200};
    static_cast<void>((*adapter)->parse_response(result, input));
  } else {
    constexpr std::string_view events[]{
        "message",
        "message_start",
        "content_block_start",
        "content_block_delta",
        "content_block_stop",
        "message_delta",
        "message_stop",
        "error",
        "future_optional",
    };
    scry::detail::ProviderDecodeState state{};
    const auto event = events[bytes.front() % std::size(events)];
    static_cast<void>((*adapter)->parse_stream_event(event, input, state));
  }
  return 0;
}
