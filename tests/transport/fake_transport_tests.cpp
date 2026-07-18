#include "support/transport/fake_transport.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <scry/error.hpp>
#include <stop_token>
#include <string>
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
