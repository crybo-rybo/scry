#pragma once

#include "core/provider.hpp"
#include "provider/anthropic_content.hpp"
#include "provider/shared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <scry/config.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Provider golden fixtures and the helpers the provider suites share.
// SCRY_ANTHROPIC_FIXTURE_DIR and SCRY_OPENAI_FIXTURE_DIR are set by
// tests/provider/CMakeLists.txt; see tests/fixtures/README.md for where these
// payloads come from and how to re-capture them.
namespace scry::test_fixtures {

[[nodiscard]] inline std::string fixture(const std::string_view directory,
                                         const std::string_view name) {
  std::ifstream input{std::string{directory} + "/" + std::string{name}};
  REQUIRE(input.good());
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] inline std::string anthropic_fixture(const std::string_view name) {
  return fixture(SCRY_ANTHROPIC_FIXTURE_DIR, name);
}

[[nodiscard]] inline std::string openai_fixture(const std::string_view name) {
  return fixture(SCRY_OPENAI_FIXTURE_DIR, name);
}

[[nodiscard]] inline Config anthropic_config() {
  return Config{
      .base_url = "https://api.anthropic.test/",
      .api_key = "sanitized-test-key",
      .model = "claude-test",
      .dialect = ProviderDialect::anthropic,
  };
}

[[nodiscard]] inline Config
openai_config(std::string base_url = "https://api.openai.test/v1") {
  return Config{
      .base_url = std::move(base_url),
      .api_key = "sanitized-key",
      .model = "chat-model",
      .dialect = ProviderDialect::openai_compatible,
  };
}

[[nodiscard]] inline JsonView json_view(const std::string_view text) {
  auto view = JsonView::parse(Json{.text = std::string{text}});
  REQUIRE(view);
  return *view;
}

// Request fixtures assert JSON meaning. They deliberately do not promise
// byte-for-byte wire spelling or member order; request_bytes_tests.cpp does.
[[nodiscard]] inline std::string canonical(const std::string_view json) {
  return json_view(json).to_json().text;
}

// The string members named `field` of each element of the array at `name`.
[[nodiscard]] inline std::vector<std::string>
member_strings(const JsonView& owner, const std::string_view name,
               const std::string_view field) {
  const auto array = owner.find(name);
  REQUIRE(array);
  std::vector<std::string> values{};
  for (std::size_t index = 0; index < array->size(); ++index) {
    const auto value = array->at(index)->find(field);
    REQUIRE(value);
    REQUIRE(value->string());
    values.emplace_back(*value->string());
  }
  return values;
}

// One Anthropic content block decoded as a stream event carries it.
[[nodiscard]] inline Result<std::optional<detail::ContentBlock>>
anthropic_content(const std::string_view json, const bool streaming_start) {
  return detail::decode_payload<detail::AnthropicContent>(json_view(json), "Anthropic")
      .and_then([streaming_start](detail::AnthropicContent content) {
        return detail::anthropic_content_block(std::move(content), streaming_start);
      });
}

[[nodiscard]] inline std::string header(const detail::TransportRequest& request,
                                        const std::string_view name) {
  for (const auto& value : request.headers) {
    if (value.name == name) {
      return value.value;
    }
  }
  return {};
}

using StreamResult = Result<std::vector<detail::ProviderEvent>>;

// One stream event decoded into a fresh sink.
[[nodiscard]] inline StreamResult decode(const detail::ProviderAdapter& adapter,
                                         const std::string_view name,
                                         const std::string_view data,
                                         detail::ProviderDecodeState& state) {
  std::vector<detail::ProviderEvent> events{};
  if (auto status = adapter.parse_stream_event(name, data, state, events); !status) {
    return std::unexpected(std::move(status.error()));
  }
  return events;
}

inline void require_protocol(const StreamResult& result) {
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::protocol);
}

inline constexpr std::string_view anthropic_message_start =
    R"({"type":"message_start","message":{"type":"message","content":[],"stop_reason":null}})";

inline void start_anthropic_message(const detail::ProviderAdapter& adapter,
                                    detail::ProviderDecodeState& state) {
  REQUIRE(decode(adapter, "message_start", anthropic_message_start, state));
}

} // namespace scry::test_fixtures
