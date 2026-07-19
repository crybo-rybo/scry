#include "tool_dispatch_test_support.hpp"

#include <string>
#include <vector>

using namespace scry::test_support;

TEST_CASE("completion callback text extraction covers empty and non-text exchanges") {
  PumpFixture fixture;
  const auto route = fixture.route(402, {});
  std::vector<std::string> observed;
  REQUIRE(route->register_completion([&observed](const scry::Completion& completion) {
    observed.push_back(completion.text);
  }));

  route->invoke(scry::detail::WorkerEvent{scry::detail::CompletionEvent{
      .turn_id = route->id(),
  }});
  route->invoke(scry::detail::WorkerEvent{scry::detail::CompletionEvent{
      .turn_id = route->id(),
      .exchange = {scry::detail::Message{
          .role = scry::detail::Role::assistant,
          .content = {tool_call("worker", "call-worker")},
      }},
  }});

  CHECK(observed == std::vector<std::string>{"", ""});
}
