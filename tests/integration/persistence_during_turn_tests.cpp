#include "runtime/test_access.hpp"
#include "support/harness_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <scry/conversation.hpp>
#include <scry/harness.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace scry::test_support;

namespace {

[[nodiscard]] scry::Config bounded_config(const std::size_t conversation_bytes) {
  auto config = test_config();
  config.limits.max_conversation_bytes = conversation_bytes;
  return config;
}

[[nodiscard]] scry::Result<scry::Harness>
fake_harness(const std::size_t conversation_bytes = 1024) {
  return scry::detail::HarnessTestAccess::create(
      bounded_config(conversation_bytes), provider(),
      std::make_unique<scry::test::FakeTransport>());
}

// anthropic_text_stream with a bare content block ahead of the real one: the
// shape the API sends when a message opens a text block it never fills. Each
// block is started and stopped before the next one begins, as the decoder
// requires.
[[nodiscard]] std::string leading_empty_text_stream(const std::string_view text) {
  auto stream = std::string{"event: message_start\ndata: "};
  stream += R"({"type":"message_start","message":{"id":"msg_empty_lead",)";
  stream += R"("type":"message","role":"assistant","content":[],)";
  stream += R"("model":"test-model","stop_reason":null,)";
  stream += R"("usage":{"input_tokens":2,"output_tokens":0}}})";
  stream += "\n\nevent: content_block_start\ndata: ";
  stream += R"({"type":"content_block_start","index":0,)";
  stream += R"("content_block":{"type":"text","text":""}})";
  stream += "\n\nevent: content_block_stop\ndata: ";
  stream += R"({"type":"content_block_stop","index":0})";
  stream += "\n\nevent: content_block_start\ndata: ";
  stream += R"({"type":"content_block_start","index":1,)";
  stream += R"("content_block":{"type":"text","text":""}})";
  stream += "\n\nevent: content_block_delta\ndata: ";
  stream += R"({"type":"content_block_delta","index":1,)";
  stream += R"("delta":{"type":"text_delta","text":")";
  stream += text;
  stream += R"("}})";
  stream += "\n\nevent: content_block_stop\ndata: ";
  stream += R"({"type":"content_block_stop","index":1})";
  stream += "\n\nevent: message_delta\ndata: ";
  stream += R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},)";
  stream += R"("usage":{"output_tokens":2}})";
  stream += "\n\nevent: message_stop\ndata: ";
  stream += R"({"type":"message_stop"})";
  stream += "\n\n";
  return stream;
}

[[nodiscard]] scry::Config openai_test_config() {
  auto config = test_config();
  config.dialect = scry::ProviderDialect::openai_compatible;
  return config;
}

void check_same_text_messages(const std::vector<scry::Message>& restored,
                              const std::vector<scry::Message>& original) {
  REQUIRE(restored.size() == original.size());
  for (std::size_t index = 0; index < original.size(); ++index) {
    CHECK(restored[index].role == original[index].role);
    const auto& expected_blocks = original[index].content;
    const auto& actual_blocks = restored[index].content;
    REQUIRE(actual_blocks.size() == expected_blocks.size());
    for (std::size_t block = 0; block < expected_blocks.size(); ++block) {
      const auto* expected = std::get_if<scry::TextBlock>(&expected_blocks[block]);
      const auto* actual = std::get_if<scry::TextBlock>(&actual_blocks[block]);
      REQUIRE(expected != nullptr);
      REQUIRE(actual != nullptr);
      CHECK(actual->text == expected->text);
    }
  }
}

// A committed history always survives the disk boundary: to_json accepts it and
// from_json restores it block for block.
void check_round_trip(const scry::Conversation& conversation) {
  const auto document = conversation.to_json();
  REQUIRE(document);
  const auto restored = scry::Conversation::from_json(*document);
  REQUIRE(restored);
  check_same_text_messages(restored->messages(), conversation.messages());
}

} // namespace

TEST_CASE("Conversation persistence excludes busy and uncommitted turn state") {
  auto conversation = scry::Conversation::from_json(
      {.text =
           R"({"messages":[{"content":[{"text":"committed","type":"text"}],"role":"user"}],"system_prompt":"prompt","version":1})"});
  REQUIRE(conversation);
  auto committed = conversation->to_json();
  REQUIRE(committed);

  auto first_harness = fake_harness();
  REQUIRE(first_harness);
  auto pending = first_harness->send(*conversation, "not committed");
  REQUIRE(pending);

  auto while_busy = conversation->to_json();
  REQUIRE(while_busy);
  CHECK(while_busy->text == committed->text);

  auto restored = scry::Conversation::from_json(*while_busy);
  REQUIRE(restored);
  auto second_harness = fake_harness();
  REQUIRE(second_harness);
  auto accepted = second_harness->send(*restored, "accepted because restored is idle");
  REQUIRE(accepted);

  auto bounded = scry::Conversation::from_json(*committed);
  REQUIRE(bounded);
  auto bounded_harness = fake_harness(15);
  REQUIRE(bounded_harness);
  auto over_limit = bounded_harness->send(*bounded, "x");
  REQUIRE_FALSE(over_limit);
  CHECK(over_limit.error().category == scry::ErrorCategory::resource_limit);
}

TEST_CASE("an empty text block never reaches the committed Anthropic history") {
  auto fixture = make_harness_fixture(
      test_config(), {scripted_exchange(leading_empty_text_stream("answered"))});

  const auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "empty block first");

  REQUIRE(completion);
  CHECK(completion->text == "answered");
  REQUIRE(fixture.conversation.message_count() == 2);
  const auto& assistant = fixture.conversation.messages().back();
  REQUIRE(assistant.content.size() == 1);
  CHECK(std::get<scry::TextBlock>(assistant.content.front()).text == "answered");
  check_round_trip(fixture.conversation);
}

TEST_CASE("an Anthropic response with only an empty text block fails the turn") {
  auto fixture = make_harness_fixture(test_config(),
                                      {scripted_exchange(anthropic_text_stream(""))});

  const auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "nothing at all");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::protocol);
  CHECK(completion.error().message == "model response contained no content");
  CHECK(fixture.conversation.empty());
}

TEST_CASE("an OpenAI response that streams no content fails the turn") {
  auto fixture = make_harness_fixture(openai_test_config(),
                                      {scripted_exchange(openai_text_stream(""))},
                                      scry::ProviderDialect::openai_compatible);

  const auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "nothing at all");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::protocol);
  CHECK(completion.error().message == "model response contained no content");
  CHECK(fixture.conversation.empty());
}

TEST_CASE("an OpenAI text completion still round-trips through persistence") {
  auto fixture = make_harness_fixture(openai_test_config(),
                                      {scripted_exchange(openai_text_stream("sunny"))},
                                      scry::ProviderDialect::openai_compatible);

  const auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "weather?");

  REQUIRE(completion);
  CHECK(completion->text == "sunny");
  REQUIRE(fixture.conversation.message_count() == 2);
  check_round_trip(fixture.conversation);
}
