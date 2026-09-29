#pragma once

#include "core/model.hpp"
#include "fixture_support.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <scry/config.hpp>
#include <string>
#include <vector>

// Requests whose exact encoded bytes request_bytes_tests.cpp pins. Between them
// they reach every member either wire struct declares, every optional both ways,
// both OpenAI content shapes, the Anthropic role merge, string escapes, and the
// sampling numbers whose spelling differs between shortest round-trip forms.
namespace scry::test_fixtures {

struct RequestBytesCase {
  std::string name{};
  Config config{};
  detail::ModelRequest request{};
};

[[nodiscard]] inline detail::ModelRequest bytes_tool_request() {
  using namespace scry::detail;
  return ModelRequest{
      .system_prompt = "Be concise",
      .history =
          std::make_shared<const std::vector<Message>>(
              std::vector<Message>{
                  Message{.role = Role::user,
                          .content = {TextBlock{.text = "Weather?"}}},
                  Message{.role = Role::assistant,
                          .content =
                              {
                                  TextBlock{.text = "Checking "},
                                  TextBlock{.text = "twice."},
                                  ToolCallBlock{
                                      .id = "call-a",
                                      .name = "weather",
                                      .arguments =
                                          Json{.text = R"({"city":"Paris","n":[1,2]})"},
                                  },
                                  ToolCallBlock{
                                      .id = "call-b",
                                      .name = "almanac",
                                      .arguments =
                                          Json{.text = R"({"zebra":1,"alpha":true})"},
                                  },
                              }},
                  Message{.role = Role::user,
                          .content =
                              {
                                  ToolResultBlock{
                                      .tool_call_id = "call-a",
                                      .result =
                                          Json{.text = R"({"high":21,"s":"a\"b"})"},
                                  },
                                  ToolResultBlock{
                                      .tool_call_id = "call-b",
                                      .result = Json{.text = R"("unavailable")"},
                                      .is_error = true,
                                  },
                              }},
              }),
      .messages =
          {
              Message{.role = Role::user, .content = {TextBlock{.text = "Tomorrow?"}}},
              Message{.role = Role::assistant,
                      .content =
                          {
                              ToolCallBlock{
                                  .id = "call-c",
                                  .name = "weather",
                                  .arguments = Json{.text = "{}"},
                              }}},
              Message{.role = Role::user,
                      .content =
                          {
                              ToolResultBlock{
                                  .tool_call_id = "call-c",
                                  .result = Json{.text = "null"},
                              }}},
          },
      .tools = std::make_shared<const std::vector<ToolDefinition>>(
          std::vector<ToolDefinition>{
              ToolDefinition{
                  .name = "weather",
                  .description = "Get \"weather\"",
                  .input_schema =
                      Json{.text = R"({"type":"object","required":["city"]})"},
              },
              ToolDefinition{
                  .name = "almanac",
                  .description = "",
                  .input_schema = Json{.text = R"({"type":"object"})"},
              },
          }),
      .sampling = SamplingConfig{.temperature = 0.7, .top_p = 0.9, .max_tokens = 64},
  };
}

[[nodiscard]] inline detail::ModelRequest bytes_text_request(std::string text) {
  using namespace scry::detail;
  return ModelRequest{
      .messages = {Message{.role = Role::user, .content = {TextBlock{.text = text}}}},
      .sampling = SamplingConfig{.max_tokens = 16},
  };
}

[[nodiscard]] inline std::vector<RequestBytesCase> request_bytes_cases() {
  using namespace scry::detail;
  std::vector<RequestBytesCase> cases{};
  for (const auto& config : {anthropic_config(), openai_config()}) {
    const std::string dialect =
        config.dialect == ProviderDialect::anthropic ? "anthropic" : "openai";
    cases.push_back({dialect + " tool history", config, bytes_tool_request()});

    auto seeded = bytes_tool_request();
    seeded.system_prompt.clear();
    seeded.tools = std::make_shared<const std::vector<ToolDefinition>>();
    seeded.sampling = SamplingConfig{.temperature = 1.0, .seed = 4294967295U};
    auto reasoning = config;
    reasoning.reasoning_mode = ReasoningMode::disabled;
    cases.push_back({dialect + " unset options", reasoning, seeded});

    cases.push_back({dialect + " escapes", config,
                     bytes_text_request("quote\" slash\\ solidus/ \b\f\n\r\t "
                                        "\x01\x1b[0m\x1f\x7f caf\xC3\xA9 "
                                        "\xF0\x9F\x8E\xB2 \xFF")});
    cases.push_back({dialect + " empty text", config, bytes_text_request("")});

    for (const double temperature :
         {0.7, 1.0, 0.1, 1e-7, 0.0, 2.0, 0.25, 1e-5, 0.0001, 0.00012,
          0.30000000000000004, 1.9999999999999998, 5e-324}) {
      auto sampled = bytes_text_request("t");
      sampled.sampling.temperature = temperature;
      sampled.sampling.top_p = temperature;
      cases.push_back(
          {dialect + " sampling " + std::to_string(temperature), config, sampled});
    }
  }
  return cases;
}

} // namespace scry::test_fixtures
