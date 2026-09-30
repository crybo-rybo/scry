#include "core/provider.hpp"
#include "request_bytes_cases.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace {

// The exact bodies the request encoders write for request_bytes_cases(). The
// fixtures in tests/fixtures compare JSON meaning; these pin every byte,
// including key order, string escapes, and number spelling.
constexpr std::array<std::string_view, 10> request_bytes{
    "{\"max_tokens\":64,\"messages\":[{\"content\":[{\"text\":\"Weather?\",\"type\":"
    "\"text\"}],\"role\":\"user\"},{\"content\":[{\"text\":\"Checking "
    "\",\"type\":\"text\"},{\"text\":\"twice.\",\"type\":\"text\"},{\"id\":\"call-a\","
    "\"input\":{\"city\":\"Paris\",\"n\":[1,2]},\"name\":\"weather\",\"type\":\"tool_"
    "use\"},{\"id\":\"call-b\",\"input\":{\"zebra\":1,\"alpha\":true},\"name\":"
    "\"almanac\",\"type\":\"tool_use\"}],\"role\":\"assistant\"},{\"content\":[{"
    "\"content\":\"{\\\"high\\\":21,\\\"s\\\":\\\"a\\\\\\\"b\\\"}\",\"is_error\":false,"
    "\"tool_use_id\":\"call-a\",\"type\":\"tool_result\"},{\"content\":"
    "\"\\\"unavailable\\\"\",\"is_error\":true,\"tool_use_id\":\"call-b\",\"type\":"
    "\"tool_result\"},{\"text\":\"Tomorrow?\",\"type\":\"text\"}],\"role\":\"user\"},{"
    "\"content\":[{\"id\":\"call-c\",\"input\":{},\"name\":\"weather\",\"type\":\"tool_"
    "use\"}],\"role\":\"assistant\"},{\"content\":[{\"content\":\"null\",\"is_error\":"
    "false,\"tool_use_id\":\"call-c\",\"type\":\"tool_result\"}],\"role\":\"user\"}],"
    "\"model\":\"claude-test\",\"stream\":true,\"system\":\"Be "
    "concise\",\"temperature\":0.7,\"tools\":[{\"description\":\"Get "
    "\\\"weather\\\"\",\"input_schema\":{\"type\":\"object\",\"required\":[\"city\"]},"
    "\"name\":\"weather\"},{\"description\":\"\",\"input_schema\":{\"type\":\"object\"}"
    ",\"name\":\"almanac\"}],\"top_p\":0.9}",
    "{\"max_tokens\":1024,\"messages\":[{\"content\":[{\"text\":\"Weather?\",\"type\":"
    "\"text\"}],\"role\":\"user\"},{\"content\":[{\"text\":\"Checking "
    "\",\"type\":\"text\"},{\"text\":\"twice.\",\"type\":\"text\"},{\"id\":\"call-a\","
    "\"input\":{\"city\":\"Paris\",\"n\":[1,2]},\"name\":\"weather\",\"type\":\"tool_"
    "use\"},{\"id\":\"call-b\",\"input\":{\"zebra\":1,\"alpha\":true},\"name\":"
    "\"almanac\",\"type\":\"tool_use\"}],\"role\":\"assistant\"},{\"content\":[{"
    "\"content\":\"{\\\"high\\\":21,\\\"s\\\":\\\"a\\\\\\\"b\\\"}\",\"is_error\":false,"
    "\"tool_use_id\":\"call-a\",\"type\":\"tool_result\"},{\"content\":"
    "\"\\\"unavailable\\\"\",\"is_error\":true,\"tool_use_id\":\"call-b\",\"type\":"
    "\"tool_result\"},{\"text\":\"Tomorrow?\",\"type\":\"text\"}],\"role\":\"user\"},{"
    "\"content\":[{\"id\":\"call-c\",\"input\":{},\"name\":\"weather\",\"type\":\"tool_"
    "use\"}],\"role\":\"assistant\"},{\"content\":[{\"content\":\"null\",\"is_error\":"
    "false,\"tool_use_id\":\"call-c\",\"type\":\"tool_result\"}],\"role\":\"user\"}],"
    "\"model\":\"claude-test\",\"stream\":true,\"temperature\":1}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"quote\\\" slash\\\\ "
    "solidus/ \\b\\f\\n\\r\\t \\u0001\\u001B[0m\\u001F\x7F caf\xC3\xA9 "
    "\xF0\x9F\x8E\xB2 "
    "\xFF\",\"type\":\"text\"}],\"role\":\"user\"}],\"model\":\"claude-test\","
    "\"stream\":true,\"temperature\":1}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"\",\"type\":\"text\"}],"
    "\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":1}",
    "{\"max_tokens\":64,\"messages\":[{\"content\":\"Be "
    "concise\",\"role\":\"system\"},{\"content\":\"Weather?\",\"role\":\"user\"},{"
    "\"content\":\"Checking "
    "twice.\",\"role\":\"assistant\",\"tool_calls\":[{\"function\":{\"arguments\":\"{"
    "\\\"city\\\":\\\"Paris\\\",\\\"n\\\":[1,2]}\",\"name\":\"weather\"},\"id\":\"call-"
    "a\",\"type\":\"function\"},{\"function\":{\"arguments\":\"{\\\"zebra\\\":1,"
    "\\\"alpha\\\":true}\",\"name\":\"almanac\"},\"id\":\"call-b\",\"type\":"
    "\"function\"}]},{\"content\":\"{\\\"high\\\":21,\\\"s\\\":\\\"a\\\\\\\"b\\\"}\","
    "\"role\":\"tool\",\"tool_call_id\":\"call-a\"},{\"content\":"
    "\"\\\"unavailable\\\"\",\"role\":\"tool\",\"tool_call_id\":\"call-b\"},{"
    "\"content\":\"Tomorrow?\",\"role\":\"user\"},{\"content\":null,\"role\":"
    "\"assistant\",\"tool_calls\":[{\"function\":{\"arguments\":\"{}\",\"name\":"
    "\"weather\"},\"id\":\"call-c\",\"type\":\"function\"}]},{\"content\":\"null\","
    "\"role\":\"tool\",\"tool_call_id\":\"call-c\"}],\"model\":\"chat-model\","
    "\"stream\":true,\"stream_options\":{\"include_usage\":true},\"temperature\":0.7,"
    "\"tools\":[{\"function\":{\"description\":\"Get "
    "\\\"weather\\\"\",\"name\":\"weather\",\"parameters\":{\"type\":\"object\","
    "\"required\":[\"city\"]}},\"type\":\"function\"},{\"function\":{\"description\":"
    "\"\",\"name\":\"almanac\",\"parameters\":{\"type\":\"object\"}},\"type\":"
    "\"function\"}],\"top_p\":0.9}",
    "{\"max_tokens\":1024,\"messages\":[{\"content\":\"Weather?\",\"role\":\"user\"},{"
    "\"content\":\"Checking "
    "twice.\",\"role\":\"assistant\",\"tool_calls\":[{\"function\":{\"arguments\":\"{"
    "\\\"city\\\":\\\"Paris\\\",\\\"n\\\":[1,2]}\",\"name\":\"weather\"},\"id\":\"call-"
    "a\",\"type\":\"function\"},{\"function\":{\"arguments\":\"{\\\"zebra\\\":1,"
    "\\\"alpha\\\":true}\",\"name\":\"almanac\"},\"id\":\"call-b\",\"type\":"
    "\"function\"}]},{\"content\":\"{\\\"high\\\":21,\\\"s\\\":\\\"a\\\\\\\"b\\\"}\","
    "\"role\":\"tool\",\"tool_call_id\":\"call-a\"},{\"content\":"
    "\"\\\"unavailable\\\"\",\"role\":\"tool\",\"tool_call_id\":\"call-b\"},{"
    "\"content\":\"Tomorrow?\",\"role\":\"user\"},{\"content\":null,\"role\":"
    "\"assistant\",\"tool_calls\":[{\"function\":{\"arguments\":\"{}\",\"name\":"
    "\"weather\"},\"id\":\"call-c\",\"type\":\"function\"}]},{\"content\":\"null\","
    "\"role\":\"tool\",\"tool_call_id\":\"call-c\"}],\"model\":\"chat-model\","
    "\"reasoning_effort\":\"none\",\"seed\":4294967295,\"stream\":true,\"stream_"
    "options\":{\"include_usage\":true},\"temperature\":1}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"quote\\\" slash\\\\ solidus/ "
    "\\b\\f\\n\\r\\t \\u0001\\u001B[0m\\u001F\x7F caf\xC3\xA9 \xF0\x9F\x8E\xB2 "
    "\xFF\",\"role\":\"user\"}],\"model\":\"chat-model\",\"stream\":true,\"stream_"
    "options\":{\"include_usage\":true},\"temperature\":1}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":1}",
    // Typed turns: the response tool is offered last and a tool call required.
    "{\"max_tokens\":64,\"messages\":[{\"content\":[{\"text\":\"Weather?\",\"type"
    "\":\"text\"}],\"role\":\"user\"},{\"content\":[{\"text\":\"Checking \",\"typ"
    "e\":\"text\"},{\"text\":\"twice.\",\"type\":\"text\"},{\"id\":\"call-a\",\"i"
    "nput\":{\"city\":\"Paris\",\"n\":[1,2]},\"name\":\"weather\",\"type\":\"tool"
    "_use\"},{\"id\":\"call-b\",\"input\":{\"zebra\":1,\"alpha\":true},\"name\":"
    "\"almanac\",\"type\":\"tool_use\"}],\"role\":\"assistant\"},{\"content\":[{"
    "\"content\":\"{\\\"high\\\":21,\\\"s\\\":\\\"a\\\\\\\"b\\\"}\",\"is_error\":"
    "false,\"tool_use_id\":\"call-a\",\"type\":\"tool_result\"},{\"content\":\""
    "\\\"unavailable\\\"\",\"is_error\":true,\"tool_use_id\":\"call-b\",\"type\":"
    "\"tool_result\"},{\"text\":\"Tomorrow?\",\"type\":\"text\"}],\"role\":\"user"
    "\"},{\"content\":[{\"id\":\"call-c\",\"input\":{},\"name\":\"weather\",\"typ"
    "e\":\"tool_use\"}],\"role\":\"assistant\"},{\"content\":[{\"content\":\"null"
    "\",\"is_error\":false,\"tool_use_id\":\"call-c\",\"type\":\"tool_result\"}],"
    "\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"system\":\"B"
    "e concise\",\"temperature\":0.7,\"tool_choice\":{\"type\":\"any\"},\"tools\""
    ":[{\"description\":\"Get \\\"weather\\\"\",\"input_schema\":{\"type\":\"obje"
    "ct\",\"required\":[\"city\"]},\"name\":\"weather\"},{\"description\":\"\",\""
    "input_schema\":{\"type\":\"object\"},\"name\":\"almanac\"},{\"description\":"
    "\"Give the \\\"final\\\" answer\",\"input_schema\":{\"additionalProperties\""
    ":false,\"properties\":{\"supported\":{\"type\":\"boolean\"}},\"required\":["
    "\"supported\"],\"type\":\"object\"},\"name\":\"respond\"}],\"top_p\":0.9}",
    "{\"max_tokens\":64,\"messages\":[{\"content\":\"Be concise\",\"role\":\"syst"
    "em\"},{\"content\":\"Weather?\",\"role\":\"user\"},{\"content\":\"Checking t"
    "wice.\",\"role\":\"assistant\",\"tool_calls\":[{\"function\":{\"arguments\":"
    "\"{\\\"city\\\":\\\"Paris\\\",\\\"n\\\":[1,2]}\",\"name\":\"weather\"},\"id"
    "\":\"call-a\",\"type\":\"function\"},{\"function\":{\"arguments\":\"{\\\"zeb"
    "ra\\\":1,\\\"alpha\\\":true}\",\"name\":\"almanac\"},\"id\":\"call-b\",\"typ"
    "e\":\"function\"}]},{\"content\":\"{\\\"high\\\":21,\\\"s\\\":\\\"a\\\\\\\"b"
    "\\\"}\",\"role\":\"tool\",\"tool_call_id\":\"call-a\"},{\"content\":\"\\\"un"
    "available\\\"\",\"role\":\"tool\",\"tool_call_id\":\"call-b\"},{\"content\":"
    "\"Tomorrow?\",\"role\":\"user\"},{\"content\":null,\"role\":\"assistant\",\""
    "tool_calls\":[{\"function\":{\"arguments\":\"{}\",\"name\":\"weather\"},\"id"
    "\":\"call-c\",\"type\":\"function\"}]},{\"content\":\"null\",\"role\":\"tool"
    "\",\"tool_call_id\":\"call-c\"}],\"model\":\"chat-model\",\"stream\":true,\""
    "stream_options\":{\"include_usage\":true},\"temperature\":0.7,\"tool_choice"
    "\":\"required\",\"tools\":[{\"function\":{\"description\":\"Get \\\"weather"
    "\\\"\",\"name\":\"weather\",\"parameters\":{\"type\":\"object\",\"required\""
    ":[\"city\"]}},\"type\":\"function\"},{\"function\":{\"description\":\"\",\"n"
    "ame\":\"almanac\",\"parameters\":{\"type\":\"object\"}},\"type\":\"function"
    "\"},{\"function\":{\"description\":\"Give the \\\"final\\\" answer\",\"name"
    "\":\"respond\",\"parameters\":{\"additionalProperties\":false,\"properties\""
    ":{\"supported\":{\"type\":\"boolean\"}},\"required\":[\"supported\"],\"type"
    "\":\"object\"}},\"type\":\"function\"}],\"top_p\":0.9}",
};

} // namespace

TEST_CASE("request encoders write byte-identical bodies") {
  const auto cases = scry::test_fixtures::request_bytes_cases();
  REQUIRE(cases.size() == request_bytes.size());
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const auto& item = cases[index];
    INFO(item.name);
    const auto adapter = scry::detail::make_provider_adapter(item.config.dialect);
    const auto encoded = adapter->make_request(item.config, item.request);
    REQUIRE(encoded);
    CHECK(encoded->body == request_bytes[index]);
  }
}

TEST_CASE("sampling numbers keep the canonical spelling in request bodies") {
  // The reflected codec writes a double in std::to_chars' shortest form (1e-07),
  // and a request body is not canonicalized after encoding, so the sampling values
  // reach the body pre-spelled. The spellings themselves are pinned with the
  // canonical writer.
  for (const auto& [value, spelling] : {std::pair{0.7, std::string_view{"0.7"}},
                                        std::pair{1e-7, std::string_view{"1E-7"}}}) {
    INFO(spelling);
    auto request = scry::test_fixtures::bytes_text_request("t");
    request.sampling.temperature = value;
    request.sampling.top_p = value;
    for (const auto& config : {scry::test_fixtures::anthropic_config(),
                               scry::test_fixtures::openai_config()}) {
      const auto adapter = scry::detail::make_provider_adapter(config.dialect);
      const auto encoded = adapter->make_request(config, request);
      REQUIRE(encoded);
      // temperature and top_p are the body's last members in either dialect.
      CHECK(encoded->body.ends_with("\"temperature\":" + std::string{spelling} +
                                    ",\"top_p\":" + std::string{spelling} + "}"));
    }
  }
}
