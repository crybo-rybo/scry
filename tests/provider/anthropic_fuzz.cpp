#include "core/provider.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

void parse_stream_payload(scry::detail::ProviderAdapter& adapter,
                          const std::string_view input,
                          scry::detail::ProviderDecodeState state) {
  static_cast<void>(adapter.parse_stream_event("message", input, state));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  const auto bytes = std::span{data, size};
  if (bytes.empty()) {
    return 0;
  }
  const auto input =
      std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  auto adapter = scry::detail::make_provider_adapter(scry::ProviderDialect::anthropic);
  if (!adapter) {
    return 0;
  }

  const scry::detail::TransportResult result{.status_code = 200};
  static_cast<void>((*adapter)->parse_response(result, input));

  parse_stream_payload(**adapter, input, {});
  scry::detail::ProviderDecodeState message_started{.message_started = true};
  parse_stream_payload(**adapter, input, message_started);

  message_started.active_content_index = 0;
  message_started.response.content.push_back(scry::detail::TextBlock{});
  parse_stream_payload(**adapter, input, message_started);

  message_started.response.content[0] = scry::detail::ToolCallBlock{};
  parse_stream_payload(**adapter, input, message_started);

  message_started.active_content_index.reset();
  message_started.finish_observed = true;
  parse_stream_payload(**adapter, input, message_started);
  return 0;
}
