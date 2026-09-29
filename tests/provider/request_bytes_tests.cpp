#include "core/provider.hpp"
#include "request_bytes_cases.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string_view>

namespace {

// The exact bodies the Glaze-backed request encoders wrote before the wire
// structs moved to the reflected codec, captured once from that encoder. The
// fixtures in tests/fixtures compare JSON meaning; these pin every byte,
// including key order, string escapes, and number spelling.
constexpr std::array<std::string_view, 34> request_bytes{
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
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":0."
    "7,\"top_p\":0.7}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":1,"
    "\"top_p\":1}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":0."
    "1,\"top_p\":0.1}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":1E-"
    "7,\"top_p\":1E-7}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":0,"
    "\"top_p\":0}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":2,"
    "\"top_p\":2}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":0."
    "25,\"top_p\":0.25}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":1E-"
    "5,\"top_p\":1E-5}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":0."
    "0001,\"top_p\":0.0001}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":0."
    "00012,\"top_p\":0.00012}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":0."
    "30000000000000004,\"top_p\":0.30000000000000004}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":1."
    "9999999999999998,\"top_p\":1.9999999999999998}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":[{\"text\":\"t\",\"type\":\"text\"}]"
    ",\"role\":\"user\"}],\"model\":\"claude-test\",\"stream\":true,\"temperature\":5E-"
    "324,\"top_p\":5E-324}",
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
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":0.7,\"top_p\":0.7}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":1,\"top_p\":1}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":0.1,\"top_p\":0.1}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":1E-7,\"top_p\":1E-7}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":0,\"top_p\":0}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":2,\"top_p\":2}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":0.25,\"top_p\":0.25}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":1E-5,\"top_p\":1E-5}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":0.0001,\"top_p\":0.0001}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":0.00012,\"top_p\":0.00012}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":0.30000000000000004,\"top_p\":0.30000000000000004}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":1.9999999999999998,\"top_p\":1.9999999999999998}",
    "{\"max_tokens\":16,\"messages\":[{\"content\":\"t\",\"role\":\"user\"}],\"model\":"
    "\"chat-model\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
    "\"temperature\":5E-324,\"top_p\":5E-324}",
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
