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

TEST_CASE("scripted transport streams chunks and captures requests") {
  scry::test::FakeTransport transport;
  transport.enqueue(scry::test::ScriptedExchange{
      .body_chunks = {"hel", "lo"},
      .result =
          scry::detail::TransportResult{
              .status_code = 200,
              .provider_request_id = "request-1",
          },
  });
  std::string body;
  scry::detail::BodyChunkSink sink{
      [&body](const std::string_view chunk) -> scry::Status {
        body.append(chunk);
        return {};
      }};
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(request(), shutdown.get_token(), cancelled, sink);

  REQUIRE(result);
  CHECK(result->status_code == 200);
  CHECK(result->provider_request_id == "request-1");
  CHECK(body == "hello");
  REQUIRE(transport.requests().size() == 1);
  CHECK(transport.requests().front().body == R"({"message":"hello"})");
  CHECK(transport.remaining() == 0);
}

TEST_CASE("scripted transport propagates a response consumer failure") {
  scry::test::FakeTransport transport;
  transport.enqueue(scry::test::ScriptedExchange{
      .body_chunks = {"first", "second"},
  });
  scry::detail::BodyChunkSink sink{[](std::string_view) -> scry::Status {
    return std::unexpected(scry::Error{
        .category = scry::ErrorCategory::resource_limit,
        .message = "bounded sink",
    });
  }};
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(request(), shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::resource_limit);
  CHECK(transport.remaining() == 0);
}

TEST_CASE("scripted transport observes cancellation before consuming a script") {
  scry::test::FakeTransport transport;
  transport.enqueue({});
  scry::detail::BodyChunkSink sink{[](std::string_view) -> scry::Status { return {}; }};
  std::stop_source shutdown;
  const std::atomic cancelled{true};

  const auto result =
      transport.perform(request(), shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::cancelled);
  CHECK(transport.remaining() == 1);
  CHECK(transport.requests().empty());
}

TEST_CASE("scripted transport reports exhausted scripts") {
  scry::test::FakeTransport transport;
  scry::detail::BodyChunkSink sink{[](std::string_view) -> scry::Status { return {}; }};
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(request(), shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::invalid_state);
}

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

TEST_CASE("a held exchange interrupted by shutdown reports cancellation") {
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
  const std::atomic cancelled{false};
  std::optional<scry::Result<scry::detail::TransportResult>> outcome;
  std::jthread worker{[&] {
    outcome = transport.perform(request(), shutdown.get_token(), cancelled, sink);
  }};

  transport.wait_for_call(1);
  shutdown.request_stop();
  worker.join();

  REQUIRE(outcome);
  REQUIRE_FALSE(*outcome);
  CHECK(outcome->error().category == scry::ErrorCategory::cancelled);
  CHECK(outcome->error().message == "scripted transport cancelled");
  CHECK(body.empty());
}
