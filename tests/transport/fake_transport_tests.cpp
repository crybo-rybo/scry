#include "support/transport/fake_transport.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <scry/error.hpp>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace {

[[nodiscard]] scry::detail::TransportRequest request() {
  return scry::detail::TransportRequest{
      .url = "https://example.invalid/messages",
      .headers = {{"content-type", "application/json"}},
      .body = R"({"message":"hello"})",
  };
}

} // namespace

TEST_CASE("scripted transport holds an exchange until it is released") {
  scry::test::FakeTransport transport;
  transport.enqueue(scry::test::ScriptedExchange{
      .body_chunks = {"held"},
      .result =
          scry::detail::TransportResult{
              .status_code = 200,
              .provider_request_id = "request-held",
          },
      .hold = true,
  });
  std::string body;
  scry::detail::BodyChunkSink sink{
      [&body](const std::string_view chunk) -> scry::Status {
        body.append(chunk);
        return {};
      }};
  std::stop_source shutdown;
  const std::atomic cancelled{false};
  std::optional<scry::Result<scry::detail::TransportResult>> outcome;
  std::jthread worker{[&] {
    outcome = transport.perform(request(), shutdown.get_token(), cancelled, sink);
  }};

  // wait_for_call observes entry while the exchange is still held, so the
  // request is already recorded and no body has been delivered.
  transport.wait_for_call(1);
  CHECK(transport.calls() == 1);
  CHECK(transport.requests().size() == 1);
  CHECK_FALSE(outcome.has_value());

  transport.release();
  worker.join();

  REQUIRE(outcome);
  REQUIRE(*outcome);
  CHECK((*outcome)->provider_request_id == "request-held");
  CHECK(body == "held");
}

TEST_CASE("a held exchange ends on shutdown or turn cancellation without release") {
  for (const bool stop_harness : {true, false}) {
    CAPTURE(stop_harness);
    scry::test::FakeTransport transport;
    transport.enqueue(scry::test::ScriptedExchange{
        .body_chunks = {"never delivered"},
        .hold = true,
    });
    std::string body;
    scry::detail::BodyChunkSink sink{
        [&body](const std::string_view chunk) -> scry::Status {
          body.append(chunk);
          return {};
        }};
    std::stop_source shutdown;
    std::atomic cancelled{false};
    std::optional<scry::Result<scry::detail::TransportResult>> outcome;
    std::jthread worker{[&] {
      outcome = transport.perform(request(), shutdown.get_token(), cancelled, sink);
    }};

    transport.wait_for_call(1);
    if (stop_harness) {
      shutdown.request_stop();
    } else {
      cancelled.store(true, std::memory_order_release);
    }
    worker.join();

    REQUIRE(outcome);
    REQUIRE_FALSE(*outcome);
    CHECK(outcome->error().category == scry::ErrorCategory::cancelled);
    CHECK(outcome->error().message == "scripted transport cancelled");
    CHECK(body.empty());
  }
}
