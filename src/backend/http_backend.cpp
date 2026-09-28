#include "backend/http_backend.hpp"

#include "core/error.hpp"
#include "protocol/sse.hpp"

#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {
namespace {

struct AttemptState {
  AttemptState(const ResourceLimits& limits, ProviderEventSink& events)
      : parser(limits.max_sse_event_bytes),
        decode{.max_tool_arguments_bytes = limits.max_tool_arguments_bytes},
        sink(events) {}

  SseParser parser;
  ProviderDecodeState decode{};
  ProviderEventSink& sink;
  std::optional<ModelResponse> completed{};
  bool semantic_output_reported{false};
  // Both vectors live for the whole attempt and are cleared, never reallocated,
  // per chunk and per event, so the streaming path allocates once instead of
  // once per received event.
  std::vector<SseEvent> sse_events{};
  std::vector<ProviderEvent> provider_events{};
};

// Provider events are consumed exactly once, so they move to the sink and the
// attempt's vector is reused. The switch into semantic output is reported ahead
// of the events decoded with it, and the completion is kept back because it is
// perform()'s return value rather than a streamed event.
[[nodiscard]] Status publish_decoded_events(AttemptState& state) {
  if (state.decode.semantic_output_consumed && !state.semantic_output_reported) {
    state.semantic_output_reported = true;
    if (auto status = state.sink(ProviderSemanticOutput{}); !status) {
      return status;
    }
  }
  for (auto& event : state.provider_events) {
    if (auto* completed = std::get_if<ProviderCompleted>(&event)) {
      if (state.completed) {
        return std::unexpected(
            make_error(ErrorCategory::protocol,
                       "provider stream emitted more than one completion"));
      }
      state.completed = std::move(completed->response);
      continue;
    }
    if (auto status = state.sink(std::move(event)); !status) {
      return status;
    }
  }
  return {};
}

[[nodiscard]] Status consume_sse_events(const ProviderAdapter& provider,
                                        AttemptState& state) {
  for (const auto& event : state.sse_events) {
    state.provider_events.clear();
    if (auto status = provider.parse_stream_event(event.name, event.data, state.decode,
                                                  state.provider_events);
        !status) {
      return status;
    }
    if (auto status = publish_decoded_events(state); !status) {
      return status;
    }
  }
  return {};
}

[[nodiscard]] Status consume_stream_chunk(const ProviderAdapter& provider,
                                          AttemptState& state,
                                          const std::string_view chunk) {
  state.sse_events.clear();
  if (auto status = state.parser.push(chunk, state.sse_events); !status) {
    return status;
  }
  return consume_sse_events(provider, state);
}

[[nodiscard]] Result<ModelResponse> finish_stream(const ProviderAdapter& provider,
                                                  AttemptState& state) {
  state.sse_events.clear();
  if (auto status = state.parser.finish(state.sse_events); !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (auto status = consume_sse_events(provider, state); !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (!state.completed) {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "provider stream ended without a completion event"));
  }
  return std::move(*state.completed);
}

} // namespace

HttpStreamBackend::HttpStreamBackend(std::unique_ptr<ProviderAdapter> provider,
                                     std::unique_ptr<Transport> transport) noexcept
    : provider_(std::move(provider)), transport_(std::move(transport)) {}

Result<ModelResponse> HttpStreamBackend::perform(const Config& config,
                                                 const ModelRequest& request,
                                                 std::stop_token shutdown,
                                                 const std::atomic<bool>& cancelled,
                                                 ProviderEventSink& sink) {
  auto transport_request = provider_->make_request(config, request);
  if (!transport_request) {
    return std::unexpected(std::move(transport_request.error()));
  }

  AttemptState state{config.limits, sink};
  BodyChunkSink body_sink{
      [provider = provider_.get(), &state](const std::string_view chunk) -> Status {
        return consume_stream_chunk(*provider, state, chunk);
      }};

  auto result = transport_->perform(*transport_request, std::move(shutdown), cancelled,
                                    body_sink);
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  auto response = finish_stream(*provider_, state);
  // A request identifier in the stream itself wins; the transport's response
  // header is the fallback.
  if (response && response->provider_request_id.empty()) {
    response->provider_request_id = std::move(result->provider_request_id);
  }
  return response;
}

} // namespace scry::detail
