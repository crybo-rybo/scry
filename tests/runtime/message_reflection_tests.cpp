#include <catch2/catch_test_macros.hpp>
#include <scry/message.hpp>
#include <scrydantic/codec.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <variant>
#include <vector>

static_assert(scrydantic::Encodable<scry::Message>);
static_assert(scrydantic::Decodable<scry::Message>);
static_assert(scrydantic::Encodable<scry::ContentBlock>);

TEST_CASE("the public message model round-trips through Scrydantic") {
  const std::vector<scry::Message> messages{
      scry::Message{.role = scry::Role::user,
                    .content = {scry::TextBlock{.text = "Weather in \"Paris\"?"}}},
      scry::Message{
          .role = scry::Role::assistant,
          .content =
              {
                  scry::TextBlock{.text = "Checking."},
                  scry::ToolCallBlock{
                      .id = "call-1",
                      .name = "weather",
                      .arguments = scrydantic::Json{.text = R"({"city":"Paris"})"},
                  },
              }},
      scry::Message{.role = scry::Role::user,
                    .content = {scry::ToolResultBlock{
                        .tool_call_id = "call-1",
                        .result = scrydantic::Json{.text = R"({"high":21})"},
                        .is_error = true,
                    }}},
  };

  const auto encoded = scrydantic::encode(messages);
  REQUIRE(encoded);
  CHECK(
      encoded->text ==
      R"([{"content":[{"text":"Weather in \"Paris\"?","type":"text"}],"role":"user"},)"
      R"({"content":[{"text":"Checking.","type":"text"},{"arguments":{"city":"Paris"},)"
      R"("id":"call-1","name":"weather","type":"tool_call"}],"role":"assistant"},)"
      R"({"content":[{"is_error":true,"result":{"high":21},"tool_call_id":"call-1",)"
      R"("type":"tool_result"}],"role":"user"}])");

  const auto decoded = scrydantic::decode<std::vector<scry::Message>>(*encoded);
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

  const auto reencoded = scrydantic::encode(*decoded);
  REQUIRE(reencoded);
  CHECK(reencoded->text == encoded->text);
}
