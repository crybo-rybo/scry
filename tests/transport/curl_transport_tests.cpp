#include "support/transport/loopback_server.hpp"
#include "transport/curl_transport.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <scry/error.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace {

using scry::ErrorCategory;
using scry::detail::BodyChunkSink;
using scry::detail::CurlTransport;
using scry::detail::TransportRequest;

[[nodiscard]] std::string response(const std::string_view status,
                                   const std::string_view headers,
                                   const std::string_view body) {
  return "HTTP/1.1 " + std::string{status} + "\r\n" + std::string{headers} +
         "Content-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + std::string{body};
}

[[nodiscard]] TransportRequest request(const std::string& url) {
  return TransportRequest{
      .url = url,
      .headers =
          {
              {"Content-Type", "application/json"},
              {"x-api-key", "test-key-never-log"},
          },
      .body = R"({"prompt":"request-body-never-log"})",
  };
}

[[nodiscard]] BodyChunkSink append_to(std::string& body) {
  return BodyChunkSink{[&body](const std::string_view chunk) -> scry::Status {
    body.append(chunk);
    return {};
  }};
}

struct InterruptedTransfer {
  scry::Result<scry::detail::TransportResult> result;
  std::chrono::steady_clock::duration shutdown_elapsed{};
};

[[nodiscard]] InterruptedTransfer interrupt_during_transfer(const bool stop_harness) {
  using namespace std::chrono_literals;
  scry::test::LoopbackServer server{response("200 OK", "", "body"), true};
  CurlTransport transport;
  auto held_request = request(server.url());
  held_request.timeouts.transfer = 2s;
  held_request.timeouts.shutdown = 50ms;
  std::string body;
  auto sink = append_to(body);
  std::stop_source shutdown;
  std::atomic cancelled{false};
  std::optional<scry::Result<scry::detail::TransportResult>> outcome;
  std::jthread worker{[&] {
    outcome = transport.perform(held_request, shutdown.get_token(), cancelled, sink);
  }};
  server.wait_until_request();
  if (stop_harness) {
    shutdown.request_stop();
  } else {
    cancelled.store(true, std::memory_order_release);
  }
  const auto started = std::chrono::steady_clock::now();
  worker.join();
  return {
      .result = std::move(*outcome),
      .shutdown_elapsed = std::chrono::steady_clock::now() - started,
  };
}

} // namespace

TEST_CASE("curl transport posts request data and returns response metadata") {
  scry::test::LoopbackServer server{
      response("200 OK", "Content-Type: application/json\r\nrequest-id: req-123\r\n",
               R"({"text":"hello"})")};
  CurlTransport transport;
  std::string body;
  auto sink = append_to(body);
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result = transport.perform(request(server.url("/v1/messages")),
                                        shutdown.get_token(), cancelled, sink);

  REQUIRE(result);
  CHECK(result->status_code == 200);
  CHECK(result->provider_request_id == "req-123");
  CHECK(body == R"({"text":"hello"})");
  const auto posted = server.request();
  CHECK(posted.find("POST /v1/messages HTTP/1.1") != std::string::npos);
  CHECK(posted.find("Content-Type: application/json") != std::string::npos);
  CHECK(posted.ends_with(R"({"prompt":"request-body-never-log"})"));
}

TEST_CASE("curl transport enforces declared response size before the sink") {
  scry::test::LoopbackServer server{response("200 OK", "", "response-too-large")};
  CurlTransport transport;
  auto bounded_request = request(server.url());
  bounded_request.limits.max_response_bytes = 4;
  std::size_t sink_calls{};
  BodyChunkSink sink{[&sink_calls](std::string_view) -> scry::Status {
    ++sink_calls;
    return {};
  }};
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(bounded_request, shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);
  CHECK(sink_calls == 0);
}

TEST_CASE("curl transport enforces streamed response size before each sink call") {
  const std::string chunked_response{
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
      "Connection: close\r\n\r\n5\r\nlarge\r\n0\r\n\r\n"};
  scry::test::LoopbackServer server{chunked_response};
  CurlTransport transport;
  auto bounded_request = request(server.url());
  bounded_request.limits.max_response_bytes = 4;
  std::size_t delivered_bytes{};
  BodyChunkSink sink{[&delivered_bytes](const std::string_view chunk) -> scry::Status {
    delivered_bytes += chunk.size();
    return {};
  }};
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(bounded_request, shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);
  CHECK(delivered_bytes <= bounded_request.limits.max_response_bytes);
}

TEST_CASE("curl transport sanitizes response consumer errors") {
  scry::test::LoopbackServer server{response("200 OK", "", "body")};
  CurlTransport transport;
  BodyChunkSink sink{[](std::string_view) -> scry::Status {
    return std::unexpected(scry::Error{
        .category = ErrorCategory::resource_limit,
        .message = "response-body-never-log",
        .provider_detail = "api-key-never-log",
        .retryable = true,
    });
  }};
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(request(server.url()), shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);
  CHECK(result.error().retryable);
  CHECK(result.error().message == "response consumer rejected response data");
  CHECK(result.error().provider_detail.empty());
}

TEST_CASE("curl transport rejects malformed response framing") {
  const std::string malformed{"HTTP/1.1 200 OK\r\nContent-Length: not-a-size\r\n"
                              "Connection: close\r\n\r\nbody"};
  scry::test::LoopbackServer server{malformed};
  CurlTransport transport;
  std::string body;
  auto sink = append_to(body);
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(request(server.url()), shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::protocol);
  CHECK(body.empty());
}

TEST_CASE("curl transport maps HTTP failures without leaking request data") {
  struct Case {
    std::string_view status;
    ErrorCategory category;
    bool retryable;
  };
  constexpr std::array cases{
      Case{"401 Unauthorized", ErrorCategory::authentication, false},
      Case{"429 Too Many Requests", ErrorCategory::rate_limit, true},
      Case{"503 Service Unavailable", ErrorCategory::network, true},
      Case{"400 Bad Request", ErrorCategory::protocol, false},
  };

  for (const auto& test_case : cases) {
    CAPTURE(std::string{test_case.status});
    const auto retry_after =
        test_case.category == ErrorCategory::rate_limit ? "Retry-After: 7\r\n" : "";
    scry::test::LoopbackServer server{response(
        test_case.status, "request-id: failed-request\r\n" + std::string{retry_after},
        "test-key-never-log request-body-never-log")};
    CurlTransport transport;
    std::string body;
    auto sink = append_to(body);
    std::stop_source shutdown;
    const std::atomic cancelled{false};

    const auto result =
        transport.perform(request(server.url()), shutdown.get_token(), cancelled, sink);

    REQUIRE_FALSE(result);
    CHECK(result.error().category == test_case.category);
    CHECK(result.error().retryable == test_case.retryable);
    CHECK(result.error().provider_request_id == "failed-request");
    if (test_case.category == ErrorCategory::rate_limit) {
      CHECK(result.error().retry_after == std::chrono::seconds{7});
    } else {
      CHECK_FALSE(result.error().retry_after);
    }
    CHECK(body.empty());
    CHECK(result.error().message.find("test-key-never-log") == std::string::npos);
    CHECK(result.error().message.find("request-body-never-log") == std::string::npos);
    CHECK(result.error().provider_detail.empty());
  }
}

TEST_CASE("curl transport handles cancellation signals before network IO") {
  CurlTransport transport;
  auto cancelled_request = request("http://127.0.0.1:1/");
  std::string body;
  auto sink = append_to(body);
  std::stop_source shutdown;
  const std::atomic cancelled{true};

  const auto turn_result =
      transport.perform(cancelled_request, shutdown.get_token(), cancelled, sink);
  REQUIRE_FALSE(turn_result);
  CHECK(turn_result.error().category == ErrorCategory::cancelled);

  const std::atomic active{false};
  shutdown.request_stop();
  const auto shutdown_result =
      transport.perform(cancelled_request, shutdown.get_token(), active, sink);
  REQUIRE_FALSE(shutdown_result);
  CHECK(shutdown_result.error().category == ErrorCategory::cancelled);
}

TEST_CASE("curl progress callback independently observes both cancellation signals") {
  const auto turn_result = interrupt_during_transfer(false);
  REQUIRE_FALSE(turn_result.result);
  CHECK(turn_result.result.error().category == ErrorCategory::cancelled);
  CHECK(turn_result.result.error().message == "transfer cancelled");
  CHECK(turn_result.shutdown_elapsed < std::chrono::milliseconds{500});

  const auto shutdown_result = interrupt_during_transfer(true);
  REQUIRE_FALSE(shutdown_result.result);
  CHECK(shutdown_result.result.error().category == ErrorCategory::cancelled);
  CHECK(shutdown_result.result.error().message ==
        "transfer cancelled by harness shutdown");
  CHECK(shutdown_result.shutdown_elapsed < std::chrono::milliseconds{500});
}

TEST_CASE("curl transport never forwards redirect bodies to the response sink") {
  scry::test::LoopbackServer server{
      response("302 Found", "Location: /elsewhere\r\n",
               "event: content_block_delta\r\ndata: semantic-output\r\n\r\n")};
  CurlTransport transport;
  std::string body;
  auto sink = append_to(body);
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(request(server.url()), shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::protocol);
  CHECK(body.empty());
}

TEST_CASE("curl transport parses HTTP-date Retry-After values") {
  scry::test::LoopbackServer server{
      response("429 Too Many Requests",
               "Retry-After: Wed, 21 Oct 2099 07:28:00 GMT\r\n", "retry later")};
  CurlTransport transport;
  std::string body;
  auto sink = append_to(body);
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(request(server.url()), shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::rate_limit);
  REQUIRE(result.error().retry_after);
  CHECK(*result.error().retry_after > std::chrono::hours{24});
  CHECK(body.empty());
}

TEST_CASE("curl transport preserves provider-neutral sanitized error detail") {
  scry::test::LoopbackServer server{
      response("200 OK", "request-id: header-request\r\n", "body")};
  CurlTransport transport;
  BodyChunkSink sink{[](std::string_view) -> scry::Status {
    return std::unexpected(scry::Error{
        .category = ErrorCategory::network,
        .message = "private provider message",
        .provider_detail = "anthropic:overloaded_error",
        .retryable = true,
        .provider_request_id = "body-request",
    });
  }};
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(request(server.url()), shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().message == "response consumer rejected response data");
  CHECK(result.error().provider_detail == "anthropic:overloaded_error");
  CHECK(result.error().provider_request_id == "body-request");
}

TEST_CASE("curl callbacks contain response consumer exceptions") {
  scry::test::LoopbackServer server{response("200 OK", "", "body")};
  CurlTransport transport;
  BodyChunkSink sink{[](std::string_view) -> scry::Status { throw 42; }};
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(request(server.url()), shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::protocol);
  CHECK(result.error().message == "response body processing failed");
}

TEST_CASE("curl transport rejects header injection before network IO") {
  CurlTransport transport;
  auto invalid_request = request("http://127.0.0.1:1/");
  invalid_request.headers.push_back({"x-bad", "value\r\ninjected: true"});
  std::string body;
  auto sink = append_to(body);
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(invalid_request, shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::protocol);
}

TEST_CASE("curl transport rejects invalid request state before network IO") {
  CurlTransport transport;
  std::stop_source shutdown;
  const std::atomic cancelled{false};
  std::string body;
  auto valid_sink = append_to(body);

  auto invalid_request = request("");
  auto result =
      transport.perform(invalid_request, shutdown.get_token(), cancelled, valid_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_config);

  invalid_request = request("http://127.0.0.1:1/");
  BodyChunkSink missing_sink;
  result =
      transport.perform(invalid_request, shutdown.get_token(), cancelled, missing_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_state);
}
