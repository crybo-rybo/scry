#include "scenarios.hpp"

#include "protocol/sse.hpp"

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>

namespace scry::bench {
namespace {

constexpr std::uint64_t fnv_offset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t fnv_prime = 1'099'511'628'211ULL;

void digest_byte(std::uint64_t& digest, const std::uint8_t value) noexcept {
  digest ^= value;
  digest *= fnv_prime;
}

void digest_number(std::uint64_t& digest, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    digest_byte(digest, static_cast<std::uint8_t>(value & 0xffU));
    value >>= 8U;
  }
}

void digest_text(std::uint64_t& digest, const std::string_view text) noexcept {
  digest_number(digest, static_cast<std::uint64_t>(text.size()));
  for (const char value : text) {
    digest_byte(digest, static_cast<std::uint8_t>(value));
  }
}

void observe_sse_events(const std::vector<detail::SseEvent>& events,
                        std::uint64_t& digest, std::size_t& event_count,
                        std::size_t& output_bytes, const bool validate) {
  for (const auto& event : events) {
    if (validate) {
      digest_text(digest, event.name);
      digest_text(digest, event.data);
    }
    output_bytes += event.name.size() + event.data.size();
    ++event_count;
  }
}

[[nodiscard]] std::string openai_chunk(const std::string_view choice) {
  return std::string{
             "{\"id\":\"benchmark-stream\",\"object\":\"chat.completion.chunk\","
             "\"choices\":["} +
         std::string{choice} + "]}";
}

[[nodiscard]] std::string text_choice(const std::string_view text) {
  return std::string{R"({"index":0,"delta":{"content":")"} + std::string{text} +
         R"("},"finish_reason":null})";
}

[[nodiscard]] std::string tool_choice(const std::string_view fragment) {
  return std::string{R"({"index":0,"delta":{"tool_calls":[)"} + std::string{fragment} +
         R"(]},"finish_reason":null})";
}

struct StreamObservation {
  std::uint64_t digest{fnv_offset};
  std::string completed_text{};
  std::size_t text_delta_count{};
  std::size_t tool_count{};
  std::size_t output_bytes{};
  bool completed{};
  bool valid{true};
};

void observe_response(const detail::ModelResponse& response,
                      StreamObservation& observation, const bool validate) {
  if (validate) {
    digest_number(observation.digest,
                  static_cast<std::uint64_t>(response.finish_reason));
    digest_number(observation.digest, response.usage.input_tokens);
    digest_number(observation.digest, response.usage.output_tokens);
    digest_text(observation.digest, response.provider_request_id);
  }
  for (const auto& block : response.content) {
    if (const auto* text = std::get_if<detail::TextBlock>(&block)) {
      if (validate) {
        digest_byte(observation.digest, 1);
        digest_text(observation.digest, text->text);
        observation.completed_text += text->text;
      }
      observation.output_bytes += text->text.size();
      continue;
    }
    const auto* call = std::get_if<detail::ToolCallBlock>(&block);
    if (call == nullptr) {
      observation.valid = false;
      continue;
    }
    if (validate) {
      digest_byte(observation.digest, 2);
      digest_text(observation.digest, call->id);
      digest_text(observation.digest, call->name);
      digest_text(observation.digest, call->arguments.text);
    }
    observation.output_bytes +=
        call->id.size() + call->name.size() + call->arguments.text.size();
    if (validate) {
      observation.valid = observation.valid && !call->id.empty() &&
                          !call->name.empty() && call->arguments.text.starts_with('{');
    }
    ++observation.tool_count;
  }
}

void observe_provider_events(const std::vector<detail::ProviderEvent>& events,
                             StreamObservation& observation, const bool validate) {
  for (const auto& event : events) {
    if (const auto* delta = std::get_if<detail::ProviderTextDelta>(&event)) {
      if (validate) {
        digest_byte(observation.digest, 3);
        digest_text(observation.digest, delta->text);
      }
      observation.output_bytes += delta->text.size();
      ++observation.text_delta_count;
      continue;
    }
    if (const auto* completed = std::get_if<detail::ProviderCompleted>(&event)) {
      if (validate) {
        digest_byte(observation.digest, 4);
      }
      observe_response(completed->response, observation, validate);
      observation.completed = true;
      continue;
    }
    const auto& ignored = std::get<detail::ProviderIgnoredEvent>(event);
    if (validate) {
      digest_byte(observation.digest, 5);
      digest_text(observation.digest, ignored.name);
    }
  }
}

[[nodiscard]] std::string tool_metadata_fragment(const std::size_t index) {
  const auto suffix = std::to_string(index);
  return std::string{R"({"index":)"} + suffix + R"(,"id":"call-)" + suffix +
         R"(","type":"function","function":{"name":"lookup","arguments":"{\"payload\":\""}})";
}

[[nodiscard]] std::string tool_argument_fragment(const std::size_t index) {
  const auto character = static_cast<char>('a' + static_cast<char>(index % 26));
  return std::string{R"({"index":)"} + std::to_string(index) +
         R"(,"function":{"arguments":")" + std::string(16, character) + R"("}})";
}

[[nodiscard]] std::string tool_closing_fragment(const std::size_t index) {
  return std::string{R"({"index":)"} + std::to_string(index) +
         R"(,"function":{"arguments":"\"}"}})";
}

[[nodiscard]] std::string padded_text(const std::size_t size, const std::size_t seed) {
  std::string value(size, static_cast<char>('a' + static_cast<char>(seed % 26)));
  const auto prefix = std::to_string(seed) + ':';
  value.replace(0, std::min(prefix.size(), value.size()), prefix);
  return value;
}

[[nodiscard]] std::string schema_json(const std::size_t index) {
  constexpr auto prefix = std::string_view{R"({"description":")"};
  constexpr auto suffix = std::string_view{
      R"(","properties":{"payload":{"type":"string"}},"type":"object"})"};
  constexpr std::size_t target_size = 2048;
  const auto fixed = prefix.size() + suffix.size();
  return std::string{prefix} + padded_text(target_size - fixed, index) +
         std::string{suffix};
}

[[nodiscard]] ScenarioResult
observe_encoded_request(const detail::TransportRequest& encoded,
                        const std::size_t input_bytes, const std::size_t items) {
  auto digest = fnv_offset;
  digest_text(digest, encoded.url);
  digest_text(digest, encoded.body);
  for (const auto& header : encoded.headers) {
    digest_text(digest, header.name);
    digest_text(digest, header.value);
  }
  const auto valid = !encoded.body.empty() &&
                     encoded.body.find("\"model\"") != std::string::npos &&
                     encoded.body.find("\"messages\"") != std::string::npos;
  return {
      .digest = digest,
      .input_bytes = static_cast<std::uint64_t>(input_bytes),
      .output_bytes = static_cast<std::uint64_t>(encoded.body.size()),
      .items = static_cast<std::uint64_t>(items),
      .valid = valid,
  };
}

} // namespace

SseScenario::SseScenario(const std::size_t chunk_bytes, const bool use_crlf)
    : expected_digest_(fnv_offset), chunk_bytes_(chunk_bytes), event_count_(1024) {
  const auto ending = use_crlf ? std::string_view{"\r\n"} : std::string_view{"\n"};
  for (std::size_t index = 0; index < event_count_; ++index) {
    const auto data = padded_text(48, index);
    input_.append("event: token");
    input_.append(ending);
    input_.append("data: ");
    input_.append(data);
    input_.append(ending);
    input_.append(ending);
    digest_text(expected_digest_, "token");
    digest_text(expected_digest_, data);
    output_bytes_ += std::string_view{"token"}.size() + data.size();
  }
}

ScenarioResult SseScenario::run(const bool validate) {
  detail::SseParser parser{1024 * 1024};
  auto digest = fnv_offset;
  std::size_t event_count = 0;
  std::size_t output_bytes = 0;
  const auto chunk_size = chunk_bytes_ == 0 ? input_.size() : chunk_bytes_;
  for (std::size_t offset = 0; offset < input_.size(); offset += chunk_size) {
    const auto count = std::min(chunk_size, input_.size() - offset);
    auto parsed = parser.push(std::string_view{input_}.substr(offset, count));
    if (!parsed) {
      return {};
    }
    observe_sse_events(*parsed, digest, event_count, output_bytes, validate);
  }
  auto finished = parser.finish();
  if (!finished) {
    return {};
  }
  observe_sse_events(*finished, digest, event_count, output_bytes, validate);
  auto result = ScenarioResult{
      .digest = validate ? digest : oracle_.digest,
      .input_bytes = static_cast<std::uint64_t>(input_.size()),
      .output_bytes = static_cast<std::uint64_t>(output_bytes),
      .items = static_cast<std::uint64_t>(event_count),
      .valid = event_count == event_count_ && output_bytes == output_bytes_ &&
               (!validate || digest == expected_digest_),
  };
  if (validate) {
    oracle_ = result;
  }
  return result;
}

OpenAiStreamScenario::OpenAiStreamScenario(const OpenAiStreamShape shape,
                                           const std::size_t scale)
    : adapter_(detail::make_provider_adapter(ProviderDialect::openai_compatible)) {
  if (shape == OpenAiStreamShape::text) {
    expected_fragment_count_ = scale;
    for (std::size_t index = 0; index < scale; ++index) {
      const auto text = padded_text(32, index);
      expected_text_.append(text);
      chunks_.push_back(openai_chunk(text_choice(text)));
    }
    chunks_.push_back(openai_chunk(R"({"index":0,"delta":{},"finish_reason":"stop"})"));
  } else {
    expected_tool_count_ = scale;
    constexpr std::size_t argument_fragments = 6;
    expected_fragment_count_ = scale * (argument_fragments + 2);
    for (std::size_t index = 0; index < scale; ++index) {
      chunks_.push_back(openai_chunk(tool_choice(tool_metadata_fragment(index))));
    }
    for (std::size_t fragment = 0; fragment < argument_fragments; ++fragment) {
      for (std::size_t index = 0; index < scale; ++index) {
        chunks_.push_back(openai_chunk(tool_choice(tool_argument_fragment(index))));
      }
    }
    for (std::size_t index = 0; index < scale; ++index) {
      chunks_.push_back(openai_chunk(tool_choice(tool_closing_fragment(index))));
    }
    chunks_.push_back(
        openai_chunk(R"({"index":0,"delta":{},"finish_reason":"tool_calls"})"));
  }
  chunks_.emplace_back("[DONE]");
  for (const auto& chunk : chunks_) {
    input_bytes_ += chunk.size();
  }
}

ScenarioResult OpenAiStreamScenario::run(const bool validate) {
  detail::ProviderDecodeState state{};
  StreamObservation observation{};
  for (const auto& chunk : chunks_) {
    auto events = adapter_->parse_stream_event("message", chunk, state);
    if (!events) {
      return {};
    }
    observe_provider_events(*events, observation, validate);
  }
  const auto expected_text_deltas =
      expected_tool_count_ == 0 ? expected_fragment_count_ : std::size_t{0};
  const auto valid = observation.valid && observation.completed && state.completed &&
                     (!validate || observation.completed_text == expected_text_) &&
                     observation.text_delta_count == expected_text_deltas &&
                     observation.tool_count == expected_tool_count_;
  auto result = ScenarioResult{
      .digest = validate ? observation.digest : oracle_.digest,
      .input_bytes = static_cast<std::uint64_t>(input_bytes_),
      .output_bytes = static_cast<std::uint64_t>(observation.output_bytes),
      .items = static_cast<std::uint64_t>(expected_fragment_count_),
      .valid = valid,
  };
  if (validate) {
    oracle_ = result;
  }
  return result;
}

RequestEncodingScenario::RequestEncodingScenario(const RequestDialect dialect,
                                                 const std::size_t message_count,
                                                 const std::size_t schema_count)
    : adapter_(detail::make_provider_adapter(dialect == RequestDialect::openai
                                                 ? ProviderDialect::openai_compatible
                                                 : ProviderDialect::anthropic)) {
  config_ = {
      .base_url = dialect == RequestDialect::openai ? "https://api.openai.test/v1"
                                                    : "https://api.anthropic.test",
      .api_key = "benchmark-key",
      .model = "benchmark-model",
      .dialect = dialect == RequestDialect::openai ? ProviderDialect::openai_compatible
                                                   : ProviderDialect::anthropic,
  };
  request_.system_prompt = padded_text(128, 0);
  request_.sampling = SamplingConfig{
      .temperature = 0.75,
      .top_p = 0.9,
      .max_tokens = 512,
  };
  auto history = std::make_shared<std::vector<detail::Message>>();
  history->reserve(message_count);
  for (std::size_t index = 0; index < message_count; ++index) {
    auto text = padded_text(512, index);
    input_bytes_ += text.size();
    history->push_back(detail::Message{
        .role = index % 2 == 0 ? detail::Role::user : detail::Role::assistant,
        .content = {detail::TextBlock{.text = std::move(text)}},
    });
  }
  request_.history = std::move(history);
  auto tools = std::make_shared<std::vector<detail::ToolSchema>>();
  tools->reserve(schema_count);
  for (std::size_t index = 0; index < schema_count; ++index) {
    auto schema = schema_json(index);
    input_bytes_ += schema.size();
    tools->push_back(detail::ToolSchema{
        .name = "tool-" + std::to_string(index),
        .description = padded_text(128, index),
        .input_schema = Json{.text = std::move(schema)},
    });
  }
  request_.tools = std::move(tools);
  input_bytes_ += request_.system_prompt.size();
}

ScenarioResult RequestEncodingScenario::run(const bool validate) {
  const auto items = request_.message_count() + request_.tools->size();
  auto encoded = adapter_->make_request(config_, request_);
  if (!encoded) {
    return {};
  }
  if (validate) {
    oracle_ = observe_encoded_request(*encoded, input_bytes_, items);
    return oracle_;
  }
  return {
      .digest = oracle_.digest,
      .input_bytes = static_cast<std::uint64_t>(input_bytes_),
      .output_bytes = static_cast<std::uint64_t>(encoded->body.size()),
      .items = static_cast<std::uint64_t>(items),
      .valid = oracle_.valid && !encoded->body.empty(),
  };
}

} // namespace scry::bench
