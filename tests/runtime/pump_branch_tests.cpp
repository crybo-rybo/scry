#include "tool_dispatch_test_support.hpp"

#include <string>
#include <vector>

using namespace scry::test_support;

namespace {

// The pump derives the callback text while committing, before the exchange
// moves into the Conversation, so these cases drive real deliveries rather
// than invoking the route directly.
[[nodiscard]] std::string delivered_text(PumpFixture& fixture, const std::uint64_t id,
                                         std::vector<scry::detail::Message> exchange) {
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(id, {});
  pump.add_route(route);
  std::string observed;
  bool delivered = false;
  REQUIRE(route->register_completion(
      [&observed, &delivered](const scry::Completion& completion) {
        observed = completion.text;
        delivered = true;
      }));
  REQUIRE(fixture.events->push(
      scry::detail::CompletionEvent{
          .turn_id = route->id(),
          .exchange = std::move(exchange),
          .finish_reason = scry::FinishReason::completed,
      },
      1024));
  static_cast<void>(pump.update({}));
  REQUIRE(delivered);
  return observed;
}

} // namespace

TEST_CASE("completion callback text extraction covers empty and non-text exchanges") {
  PumpFixture fixture;

  CHECK(delivered_text(fixture, 402, {}).empty());
  CHECK(delivered_text(fixture, 403,
                       {scry::detail::Message{
                           .role = scry::detail::Role::assistant,
                           .content = {tool_call("worker", "call-worker")},
                       }})
            .empty());
  CHECK(delivered_text(fixture, 404,
                       {scry::detail::Message{
                           .role = scry::detail::Role::assistant,
                           .content = {scry::detail::TextBlock{.text = "first "},
                                       tool_call("worker", "call-worker"),
                                       scry::detail::TextBlock{.text = "second"}},
                       }}) == "first second");
}

TEST_CASE("committing a completion moves its exchange into the Conversation") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(405, {});
  pump.add_route(route);
  REQUIRE(route->register_completion([](const scry::Completion&) {}));
  REQUIRE(fixture.events->push(completion_event(route->id()), 1024));

  static_cast<void>(pump.update({}));

  // The user message plus the committed assistant message, with content
  // intact after the move.
  REQUIRE(fixture.conversation->messages.size() == 2);
  const auto& assistant = fixture.conversation->messages.back();
  CHECK(assistant.role == scry::detail::Role::assistant);
  REQUIRE(assistant.content.size() == 1);
  CHECK(std::get<scry::detail::TextBlock>(assistant.content.front()).text == "done");
}
