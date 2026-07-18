#include "core/json_codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <scry/conversation.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::string document(const std::string_view messages,
                                   const std::string_view prompt = "\"\"",
                                   const std::string_view version = "1") {
  return "{\"messages\":" + std::string{messages} +
         ",\"system_prompt\":" + std::string{prompt} +
         ",\"version\":" + std::string{version} + "}";
}

void require_invalid_document(const std::string_view input) {
  auto conversation =
      scry::Conversation::from_json(scry::Json{.text = std::string{input}});
  REQUIRE_FALSE(conversation);
  CHECK(conversation.error().category == scry::ErrorCategory::invalid_config);
}

} // namespace

TEST_CASE("Conversation persistence uses a canonical versioned document") {
  auto created = scry::Conversation::create();
  REQUIRE(created);
  auto empty = created->to_json();
  REQUIRE(empty);
  CHECK(empty->text == R"({"messages":[],"system_prompt":"","version":1})");

  const std::string input = R"({
    "version": 1,
    "system_prompt": "Guide\ncarefully.",
    "messages": [
      {"role":"user","content":[{"type":"text","text":"Question"}]},
      {"content":[{"name":"lookup","type":"tool_call",
                   "arguments":{"z":2,"a":1},"id":"call-1"}],
       "role":"assistant"},
      {"role":"user","content":[{"result":{"z":null,"a":[true,3]},
         "type":"tool_result","tool_call_id":"call-1","is_error":false}]},
      {"role":"assistant","content":[{"type":"text","text":"Answer"}]}
    ]
  })";
  auto restored = scry::Conversation::from_json({.text = input});
  REQUIRE(restored);
  CHECK_FALSE(restored->empty());
  CHECK(restored->message_count() == 4);

  auto encoded = restored->to_json();
  REQUIRE(encoded);
  CHECK(
      encoded->text ==
      R"({"messages":[{"content":[{"text":"Question","type":"text"}],"role":"user"},{"content":[{"arguments":{"a":1,"z":2},"id":"call-1","name":"lookup","type":"tool_call"}],"role":"assistant"},{"content":[{"is_error":false,"result":{"a":[true,3],"z":null},"tool_call_id":"call-1","type":"tool_result"}],"role":"user"},{"content":[{"text":"Answer","type":"text"}],"role":"assistant"}],"system_prompt":"Guide\ncarefully.","version":1})");

  auto round_trip = scry::Conversation::from_json(*encoded);
  REQUIRE(round_trip);
  auto reencoded = round_trip->to_json();
  REQUIRE(reencoded);
  CHECK(reencoded->text == encoded->text);
}

TEST_CASE("Conversation persistence rejects malformed document structure") {
  const std::vector<std::string> invalid{
      "",
      "{",
      "[]",
      R"({"messages":[],"system_prompt":"","version":1,"extra":true})",
      R"({"messages":[],"system_prompt":"","revision":1})",
      R"({"messages":[],"version":1})",
      R"({"messages":[],"system_prompt":"","version":2})",
      R"({"messages":[],"system_prompt":"","version":-1})",
      R"({"messages":[],"system_prompt":"","version":1.0})",
      R"({"messages":[],"system_prompt":0,"version":1})",
      R"({"messages":{},"system_prompt":"","version":1})",
      document("[null]"),
      document(R"([{"content":[],"role":"user"}])"),
      document(R"([{"content":[{"text":"x","type":"text"}],"role":0}])"),
      document(R"([{"content":[],"role":"system"}])"),
      document(R"([{"content":[],"role":"user","extra":0}])"),
      document(R"([{"content":[null],"role":"user"}])"),
      document(R"([{"content":[{"text":"x"}],"role":"user"}])"),
      document(R"([{"content":[{"type":"unknown"}],"role":"user"}])"),
      document(R"([{"content":[{"text":"","type":"text"}],"role":"user"}])"),
      document(R"([{"content":[{"text":"x","type":"text","extra":0}],"role":"user"}])"),
  };
  for (const auto& value : invalid) {
    CAPTURE(value);
    require_invalid_document(value);
  }
}

TEST_CASE("Conversation persistence enforces tool block roles and shapes") {
  const std::vector<std::string> invalid{
      document(
          R"([{"content":[{"arguments":{},"id":"id","name":"tool","type":"tool_call"}],"role":"user"}])"),
      document(
          R"([{"content":[{"arguments":{},"id":"","name":"tool","type":"tool_call"}],"role":"assistant"}])"),
      document(
          R"([{"content":[{"arguments":{},"id":0,"name":"tool","type":"tool_call"}],"role":"assistant"}])"),
      document(
          R"([{"content":[{"arguments":{},"id":"id","name":"","type":"tool_call"}],"role":"assistant"}])"),
      document(
          R"([{"content":[{"arguments":{},"id":"id","name":0,"type":"tool_call"}],"role":"assistant"}])"),
      document(
          R"([{"content":[{"arguments":[],"id":"id","name":"tool","type":"tool_call"}],"role":"assistant"}])"),
      document(
          R"([{"content":[{"is_error":false,"result":null,"tool_call_id":"id","type":"tool_result"}],"role":"assistant"}])"),
      document(
          R"([{"content":[{"is_error":false,"result":null,"tool_call_id":"","type":"tool_result"}],"role":"user"}])"),
      document(
          R"([{"content":[{"is_error":false,"result":null,"tool_call_id":0,"type":"tool_result"}],"role":"user"}])"),
      document(
          R"([{"content":[{"is_error":0,"result":null,"tool_call_id":"id","type":"tool_result"}],"role":"user"}])"),
      document(
          R"([{"content":[{"is_error":false,"tool_call_id":"id","type":"tool_result"}],"role":"user"}])"),
  };
  for (const auto& value : invalid) {
    CAPTURE(value);
    require_invalid_document(value);
  }
}

TEST_CASE("Conversation persistence diagnoses inactive moved-from handles") {
  auto created = scry::Conversation::create();
  REQUIRE(created);
  auto active = std::move(*created);
  auto inactive = created->to_json();
  REQUIRE_FALSE(inactive);
  CHECK(inactive.error().category == scry::ErrorCategory::invalid_state);
  CHECK(active.empty());
}

TEST_CASE("private JSON codec canonicalizes values and validates object roots") {
  const scry::Json input{.text = R"( { "z": null, "a": [3, {"b": 2, "a": 1}] } )"};
  auto canonical = scry::detail::canonicalize_json(
      input, scry::ErrorCategory::invalid_config, "invalid JSON");
  REQUIRE(canonical);
  CHECK(canonical->text == R"({"a":[3,{"a":1,"b":2}],"z":null})");

  auto object = scry::detail::canonicalize_json_object(
      input, scry::ErrorCategory::invalid_config, "invalid JSON object");
  REQUIRE(object);
  CHECK(object->text == canonical->text);

  auto array = scry::detail::canonicalize_json_object(
      {.text = "[]"}, scry::ErrorCategory::tool, "object required");
  REQUIRE_FALSE(array);
  CHECK(array.error().category == scry::ErrorCategory::tool);
  CHECK(array.error().message == "object required");
}

TEST_CASE("private JSON codec safely quotes model-visible error strings") {
  const auto encoded = scry::detail::make_json_error_object("bad \"quote\"\nline\t\\");
  CHECK(encoded.text == R"({"error":"bad \"quote\"\nline\t\\"})");
  auto parsed = scry::detail::parse_json(encoded.text, scry::ErrorCategory::tool,
                                         "invalid error object");
  REQUIRE(parsed);
  REQUIRE(parsed->is_object());
  REQUIRE(parsed->contains("error"));
  CHECK((*parsed)["error"].get_string() == "bad \"quote\"\nline\t\\");
}
