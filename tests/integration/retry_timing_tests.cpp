#include "support/harness_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <scry/scry.hpp>
#include <utility>

// Retries wait in real time, so these cases check lower bounds, ordering, and
// counts. An upper bound is only a guard against a hang, never a measure of
// precision, because a loaded runner can delay any wake.
using namespace std::chrono_literals;
using namespace scry::test_support;

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] scry::Config retry_config() {
  auto config = test_config();
  config.retry.max_attempts = 3;
  config.retry.initial_backoff = 100ms;
  config.retry.max_backoff = 10s;
  config.retry.max_elapsed = 30s;
  return config;
}

// A 503 is a retryable network failure.
[[nodiscard]] ScriptedResponse unavailable() {
  return {.status = 503, .body_chunks = {"unavailable"}};
}

} // namespace

TEST_CASE("retry backoff doubles from one failed attempt to the next") {
  auto fixture = make_harness_fixture(
      retry_config(),
      {unavailable(), unavailable(), scripted_response(anthropic_text_stream("done"))});
  std::optional<scry::Result<scry::Completion>> finished;

  const auto start = Clock::now();
  auto turn =
      fixture.harness.send(fixture.conversation, "retry twice",
                           {
                               .on_finished =
                                   [&finished](scry::Result<scry::Completion> result) {
                                     finished = std::move(result);
                                   },
                           });
  REQUIRE(turn);
  // The worker retries without the host, so the test thread can time each
  // attempt as the server receives it. A wait ends no earlier than the arrival
  // it waits for, so each measurement is an upper estimate of that arrival.
  REQUIRE(fixture.server.wait_for_request(2));
  const auto second_attempt = Clock::now() - start;
  REQUIRE(fixture.server.wait_for_request(3));
  const auto third_attempt = Clock::now() - start;
  REQUIRE(pump_until(fixture.harness, [&finished] { return finished.has_value(); }));

  REQUIRE(*finished);
  CHECK((*finished)->text == "done");
  CHECK((*finished)->attempt_count == 3);
  // The first backoff is 100 ms and the second doubles to 200 ms.
  CHECK(second_attempt >= 100ms);
  CHECK(third_attempt >= 300ms);
}

// Retry-After has a resolution of whole seconds, so this case costs one second.
// It is the only end-to-end proof that the server's delay reaches the wait: the
// transport tests parse the header, and the machine tests schedule a given
// value.
TEST_CASE("Retry-After lengthens the backoff end to end") {
  auto fixture = make_harness_fixture(
      retry_config(), {
                          {
                              .status = 429,
                              .headers = {{.name = "Retry-After", .value = "1"}},
                              .body_chunks = {anthropic_error_body("rate_limit_error")},
                          },
                          scripted_response(anthropic_text_stream("done")),
                      });

  const auto start = Clock::now();
  const auto completion = fixture.harness.send_and_wait(fixture.conversation, "wait");
  const auto elapsed = Clock::now() - start;

  REQUIRE(completion);
  CHECK(completion->attempt_count == 2);
  CHECK(fixture.server.requests().size() == 2);
  // The 100 ms exponential backoff alone would retry long before this.
  CHECK(elapsed >= 1s);
}

TEST_CASE("max_backoff caps the delay that Retry-After asks for") {
  auto config = retry_config();
  config.retry.initial_backoff = 1ms;
  config.retry.max_backoff = 5ms;
  auto fixture = make_harness_fixture(
      config, {
                  {
                      .status = 429,
                      .headers = {{.name = "Retry-After", .value = "10"}},
                      .body_chunks = {anthropic_error_body("rate_limit_error")},
                  },
                  scripted_response(anthropic_text_stream("done")),
              });

  const auto start = Clock::now();
  const auto completion = fixture.harness.send_and_wait(fixture.conversation, "cap");
  const auto elapsed = Clock::now() - start;

  REQUIRE(completion);
  CHECK(completion->attempt_count == 2);
  // An honoured Retry-After takes ten seconds, so this bound only separates a
  // capped wait from an uncapped one.
  CHECK(elapsed < 5s);
}

TEST_CASE("the elapsed-time cap ends retrying") {
  auto config = retry_config();
  config.retry.max_attempts = 10;
  config.retry.initial_backoff = 200ms;
  config.retry.max_elapsed = 500ms;
  auto fixture =
      make_harness_fixture(config, {unavailable(), unavailable(), unavailable()});

  const auto start = Clock::now();
  const auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "exhaust the window");
  const auto elapsed = Clock::now() - start;

  // The retry window ends 500 ms after the turn starts. Attempt 1 fails at once,
  // so its 200 ms backoff wakes inside the window and attempt 2 runs. Attempt 2
  // fails at about 200 ms, and its backoff doubles to 400 ms, which wakes past
  // the window. So the turn ends with the second failure and does not wait for
  // that wake. Only a delay of 300 ms in attempt 1 could move its wake past the
  // window too.
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::network);
  CHECK(completion.error().retryable);
  CHECK(completion.error().http_status == 503);
  CHECK(completion.error().attempt == 2);
  CHECK(fixture.server.requests().size() == 2);
  CHECK(fixture.server.remaining() == 1);
  CHECK(elapsed >= 200ms);
  CHECK(fixture.conversation.empty());
}
