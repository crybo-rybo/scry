#include "core/provider.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

void parse_stream_payload(scry::detail::ProviderAdapter& adapter,
                          const std::string_view event_name,
                          const std::string_view input,
                          scry::detail::ProviderDecodeState state) {
  static_cast<void>(adapter.parse_stream_event(event_name, input, state));
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
  auto adapter =
      scry::detail::make_provider_adapter(scry::ProviderDialect::openai_compatible);
  if (!adapter) {
    return 0;
  }

  parse_stream_payload(**adapter, "message", input, {});
  parse_stream_payload(**adapter, "error", input, {});
  parse_stream_payload(**adapter, "future_optional", input, {});

  scry::detail::ProviderDecodeState active{};
  auto& openai = active.dialect.emplace<scry::detail::OpenAiProviderDecodeState>();
  openai.chunk_id = "chatcmpl-fuzz";
  openai.tool_calls[0] = scry::detail::OpenAiToolDecodeState{
      .id = "call",
      .name = "tool",
      .type = "function",
  };
  active.max_tool_arguments_bytes = 1024;
  parse_stream_payload(**adapter, "message", input, active);

  openai.tool_calls.clear();
  openai.finish_observed = true;
  openai.tools_finalized = true;
  parse_stream_payload(**adapter, "message", input, active);
  parse_stream_payload(**adapter, "future_optional", input, active);
  return 0;
}
