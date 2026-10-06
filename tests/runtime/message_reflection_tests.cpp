#include <catch2/catch_test_macros.hpp>
#include <scry/json.hpp>
#include <scry/message.hpp>
#include <scry/reflection.hpp>
#include <string>
#include <variant>
#include <vector>

static_assert(scry::reflection::Encodable<scry::Message>);
static_assert(scry::reflection::Decodable<scry::Message>);
static_assert(scry::reflection::Encodable<scry::ContentBlock>);

TEST_CASE("the public message model round-trips through the reflected codec") {
  const std::vector<scry::Message> messages{
      scry::Message{.role = scry::Role::user,
                    .content = {scry::TextBlock{.text = "Weather in \"Paris\"?"}}},
      scry::Message{.role = scry::Role::assistant,
                    .content =
                        {
                            scry::TextBlock{.text = "Checking."},
                            scry::ToolCallBlock{
                                .id = "call-1",
                                .name = "weather",
                                .arguments = scry::Json{.text = R"({"city":"Paris"})"},
                            },
                        }},
      scry::Message{.role = scry::Role::user,
                    .content = {scry::ToolResultBlock{
                        .tool_call_id = "call-1",
                        .result = scry::Json{.text = R"({"high":21})"},
                        .is_error = true,
                    }}},
  };

  const auto encoded = scry::reflection::encode(messages);
  REQUIRE(encoded);
  CHECK(
      encoded->text ==
      R"([{"content":[{"text":"Weather in \"Paris\"?","type":"text"}],"role":"user"},)"
      R"({"content":[{"text":"Checking.","type":"text"},{"arguments":{"city":"Paris"},)"
      R"("id":"call-1","name":"weather","type":"tool_call"}],"role":"assistant"},)"
      R"({"content":[{"is_error":true,"result":{"high":21},"tool_call_id":"call-1",)"
      R"("type":"tool_result"}],"role":"user"}])");

  const auto decoded = scry::reflection::decode<std::vector<scry::Message>>(*encoded);
  REQUIRE(decoded);
  REQUIRE(decoded->size() == 3);
  CHECK((*decoded)[0].role == scry::Role::user);
  CHECK(std::get<scry::TextBlock>((*decoded)[0].content[0]).text ==
        "Weather in \"Paris\"?");
  const auto& call = std::get<scry::ToolCallBlock>((*decoded)[1].content[1]);
  CHECK(call.id == "call-1");
  CHECK(call.name == "weather");
  CHECK(call.arguments.text == R"({"city":"Paris"})");
  const auto& result = std::get<scry::ToolResultBlock>((*decoded)[2].content[0]);
  CHECK(result.tool_call_id == "call-1");
  CHECK(result.result.text == R"({"high":21})");
  CHECK(result.is_error);

  const auto reencoded = scry::reflection::encode(*decoded);
  REQUIRE(reencoded);
  CHECK(reencoded->text == encoded->text);
}

TEST_CASE("a reflected content block is selected by its type tag") {
  const auto block = scry::reflection::decode<scry::ContentBlock>(
      scry::Json{.text = R"({"type":"tool_result","tool_call_id":"a","result":null,)"
                         R"("is_error":false})"});
  REQUIRE(block);
  const auto* result = std::get_if<scry::ToolResultBlock>(&*block);
  REQUIRE(result != nullptr);
  CHECK(result->result.text == "null");

  const auto unknown = scry::reflection::decode<scry::ContentBlock>(
      scry::Json{.text = R"({"type":"image","text":"x"})"});
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error().model_message ==
        "$.type must be one of: text, tool_call, tool_result");
}
