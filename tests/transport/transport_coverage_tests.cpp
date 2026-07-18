#include "support/transport/loopback_server.hpp"
#include "transport/curl_error.hpp"
#include "transport/curl_global.hpp"
#include "transport/curl_transport.hpp"
#include "transport/transport_policy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <curl/curl.h>
#include <limits>
#include <optional>
#include <scry/error.hpp>
#include <scry/turn_id.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scry::ErrorCategory;
using scry::detail::BodyChunkSink;
using scry::detail::CurlTransport;
using scry::detail::HttpHeader;
using scry::detail::TransportRequest;

[[nodiscard]] TransportRequest request(std::string url) {
  return TransportRequest{
      .url = std::move(url),
      .body = "{}",
  };
}

[[nodiscard]] BodyChunkSink append_to(std::string& body) {
  return BodyChunkSink{[&body](const std::string_view chunk) -> scry::Status {
    body.append(chunk);
    return {};
  }};
}

[[nodiscard]] std::string http_response(const std::string_view headers,
                                        const std::string_view body) {
  return "HTTP/1.1 200 OK\r\n" + std::string{headers} +
         "Content-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + std::string{body};
}

void check_category(const scry::Error& error, const ErrorCategory category,
                    const bool retryable, const char* message) {
  CHECK(error.category == category);
  CHECK(error.retryable == retryable);
  CHECK(error.message == message);
}

} // namespace

TEST_CASE("curl error classification honors callback and cancellation precedence") {
  using scry::detail::curl_error::AbortCause;
  using scry::detail::curl_error::classify;

  const scry::Error callback{
      .category = ErrorCategory::tool,
      .message = "consumer failed",
      .provider_detail = "provider:safe_code",
      .retryable = true,
      .retry_after = std::chrono::milliseconds{17},
      .turn_id = scry::TurnId{42},
      .attempt = 3,
      .provider_request_id = "body-request",
  };
  const auto preserved =
      classify(CURLE_FILESIZE_EXCEEDED, callback, AbortCause::harness_shutdown);
  CHECK(preserved.category == callback.category);
  CHECK(preserved.message == callback.message);
  CHECK(preserved.provider_detail == callback.provider_detail);
  CHECK(preserved.retryable == callback.retryable);
  CHECK(preserved.retry_after == callback.retry_after);
  CHECK(preserved.turn_id == callback.turn_id);
  CHECK(preserved.attempt == callback.attempt);
  CHECK(preserved.provider_request_id == callback.provider_request_id);

  const auto turn =
      classify(CURLE_COULDNT_CONNECT, std::nullopt, AbortCause::turn_cancelled);
  check_category(turn, ErrorCategory::cancelled, false, "transfer cancelled");

  const auto shutdown = classify(CURLE_OK, std::nullopt, AbortCause::harness_shutdown);
  check_category(shutdown, ErrorCategory::cancelled, false,
                 "transfer cancelled by harness shutdown");
}

TEST_CASE("curl error classification covers resource TLS protocol and network codes") {
  using scry::detail::curl_error::AbortCause;
  using scry::detail::curl_error::classify;

  struct Case {
    CURLcode code;
    ErrorCategory category;
    const char* message;
    bool retryable;
  };
  constexpr std::array cases{
      Case{CURLE_FILESIZE_EXCEEDED, ErrorCategory::resource_limit,
           "response exceeds configured limit", false},
      Case{CURLE_PEER_FAILED_VERIFICATION, ErrorCategory::protocol,
           "TLS verification failed", false},
      Case{CURLE_SSL_CERTPROBLEM, ErrorCategory::protocol, "TLS verification failed",
           false},
      Case{CURLE_SSL_CACERT_BADFILE, ErrorCategory::protocol, "TLS verification failed",
           false},
      Case{CURLE_SSL_ISSUER_ERROR, ErrorCategory::protocol, "TLS verification failed",
           false},
      Case{CURLE_WEIRD_SERVER_REPLY, ErrorCategory::protocol, "invalid server response",
           false},
      Case{CURLE_UNSUPPORTED_PROTOCOL, ErrorCategory::protocol,
           "invalid server response", false},
      Case{CURLE_URL_MALFORMAT, ErrorCategory::protocol, "invalid server response",
           false},
      Case{CURLE_COULDNT_CONNECT, ErrorCategory::network, "network transfer failed",
           true},
      Case{CURLE_OK, ErrorCategory::network, "network transfer failed", true},
  };

  for (const auto& test_case : cases) {
    CAPTURE(test_case.code);
    const auto error = classify(test_case.code, std::nullopt, AbortCause::none);
    check_category(error, test_case.category, test_case.retryable, test_case.message);
  }
}

TEST_CASE("Retry-After accepts numeric values and saturates large delays") {
  using scry::detail::curl_error::retry_after;

  const auto fallback = retry_after({
      HttpHeader{.name = "Retry-After", .value = "not-a-date"},
      HttpHeader{.name = "ReTrY-AfTeR", .value = "7"},
  });
  REQUIRE(fallback);
  CHECK(*fallback == std::chrono::seconds{7});
  CHECK(retry_after({HttpHeader{.name = "retry-after", .value = "0"}}) ==
        std::chrono::milliseconds::zero());

  const auto saturated = retry_after(
      {HttpHeader{.name = "retry-after",
                  .value = std::to_string(std::numeric_limits<std::size_t>::max())}});
  REQUIRE(saturated);
  constexpr auto maximum = std::numeric_limits<std::chrono::milliseconds::rep>::max();
  CHECK(saturated->count() == (maximum / 1000) * 1000);

  CHECK_FALSE(retry_after({}));
  CHECK_FALSE(
      retry_after({HttpHeader{.name = "content-type", .value = "text/event-stream"},
                   HttpHeader{.name = "retry-after", .value = "still-not-a-date"}}));
}

TEST_CASE("Retry-After clamps past dates and accepts future HTTP dates") {
  using scry::detail::curl_error::retry_after;

  const auto past = retry_after(
      {HttpHeader{.name = "retry-after", .value = "Thu, 01 Jan 1970 00:00:00 GMT"}});
  REQUIRE(past);
  CHECK(*past == std::chrono::milliseconds::zero());

  const auto future = retry_after(
      {HttpHeader{.name = "retry-after", .value = "Wed, 21 Oct 2099 07:28:00 GMT"}});
  REQUIRE(future);
  CHECK(*future > std::chrono::hours{24});
}

TEST_CASE("response policy handles status transitions and identifier boundaries") {
  using scry::detail::transport_policy::ResponseState;

  ResponseState response{.limit = 2048};
  REQUIRE(response.accept_header("HTTP/2 204"));
  CHECK(response.deliver_body);

  const std::string identifier(256, 'r');
  REQUIRE(response.accept_header(" X-ReQuEsT-Id : " + identifier + " \r\n"));
  REQUIRE(response.accept_header("Content-Length: 2048\r\n"));
  CHECK(response.provider_request_id == identifier);
  REQUIRE(response.headers.size() == 2);

  REQUIRE(response.accept_header("HTTP/1.1 101 Switching Protocols\r\n"));
  CHECK_FALSE(response.deliver_body);
  CHECK(response.headers.empty());
  CHECK(response.provider_request_id.empty());
  REQUIRE(response.accept_header(" \t\r\n"));
}

TEST_CASE("response policy rejects malformed status and byte accounting overflow") {
  using scry::detail::transport_policy::ResponseState;

  for (const auto line : {"HTTP/1.1 ", "HTTP/1.1 200x OK"}) {
    CAPTURE(line);
    ResponseState response{.limit = 128};
    const auto result = response.accept_header(line);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }

  ResponseState metadata{.limit = 3};
  auto result = metadata.accept_header("four");
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);

  ResponseState corrupt{.limit = 4, .received_bytes = 5};
  result = corrupt.account_body(0);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);

  ResponseState bounded{.limit = 8, .received_bytes = 6};
  result = bounded.account_body(3);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);
}

TEST_CASE("transport policy covers numeric and provider-detail boundaries") {
  using namespace scry::detail::transport_policy;

  CHECK(parse_size(std::to_string(std::numeric_limits<std::size_t>::max())) ==
        std::numeric_limits<std::size_t>::max());
  CHECK_FALSE(parse_size("+1"));
  CHECK_FALSE(parse_size(" 1"));

  const std::string maximum_detail = "p:" + std::string(126, 'x');
  CHECK(sanitize_provider_detail(maximum_detail) == maximum_detail);
  CHECK(sanitize_provider_detail("p:" + std::string(127, 'x')).empty());
  CHECK(sanitize_provider_detail("p:unsafe value").empty());

  CHECK_FALSE(header_name_equal("request-ix", "request-id"));
  CHECK(is_request_id_header("x-request-id"));
  CHECK(is_request_id_header("anthropic-request-id"));
}

TEST_CASE("HTTP error policy covers server-range boundaries") {
  using scry::detail::transport_policy::http_error;

  CHECK(http_error(499, "").category == ErrorCategory::protocol);
  CHECK(http_error(500, "").category == ErrorCategory::network);
  CHECK(http_error(599, "").category == ErrorCategory::network);
  CHECK(http_error(600, "").category == ErrorCategory::protocol);
}

TEST_CASE("curl global and transport leases remain usable after moves") {
  scry::detail::CurlGlobalLease shared;
  REQUIRE_FALSE(shared.error());
  scry::detail::CurlGlobalLease moved{std::move(shared)};
  REQUIRE_FALSE(moved.error());
  scry::detail::CurlGlobalLease assigned;
  assigned = std::move(moved);
  REQUIRE_FALSE(assigned.error());

  CurlTransport original;
  CurlTransport moved_transport{std::move(original)};
  CurlTransport transport;
  transport = std::move(moved_transport);
  REQUIRE(transport.status());
}

TEST_CASE("curl runtime rejects capabilities that cannot honor host shutdown") {
  using scry::detail::CurlRuntimeCapabilities;
  using scry::detail::validate_curl_runtime_capabilities;

  REQUIRE(validate_curl_runtime_capabilities({
      .version_number = CURL_VERSION_BITS(7, 84, 0),
      .thread_safe = true,
      .asynchronous_dns = true,
  }));

  const auto old_version = validate_curl_runtime_capabilities({
      .version_number = CURL_VERSION_BITS(7, 83, 0),
      .thread_safe = true,
      .asynchronous_dns = true,
  });
  REQUIRE_FALSE(old_version);
  CHECK(old_version.error().category == ErrorCategory::invalid_config);

  const auto unsafe_global = validate_curl_runtime_capabilities({
      .version_number = CURL_VERSION_BITS(7, 84, 0),
      .thread_safe = false,
      .asynchronous_dns = true,
  });
  REQUIRE_FALSE(unsafe_global);
  CHECK(unsafe_global.error().category == ErrorCategory::invalid_config);

  const auto blocking_resolver = validate_curl_runtime_capabilities({
      .version_number = CURL_VERSION_BITS(7, 84, 0),
      .thread_safe = true,
      .asynchronous_dns = false,
  });
  REQUIRE_FALSE(blocking_resolver);
  CHECK(blocking_resolver.error().category == ErrorCategory::invalid_config);
}

TEST_CASE("curl transport resets interim metadata and supports disabled TLS checks") {
  using namespace std::chrono_literals;

  const std::string raw_response{"HTTP/1.1 100 Continue\r\n"
                                 "request-id: interim-request\r\n\r\n"
                                 "HTTP/1.1 200 OK\r\n"
                                 "request-id: final-request\r\n"
                                 "Content-Length: 2\r\nConnection: close\r\n\r\nok"};
  scry::test::LoopbackServer server{raw_response};
  CurlTransport transport;
  auto local_request = request(server.url());
  local_request.tls_verify_peer = false;
  local_request.timeouts.connect = std::chrono::milliseconds::max();
  local_request.timeouts.transfer = std::chrono::milliseconds::max();
  local_request.timeouts.shutdown = std::chrono::milliseconds::max();
  std::string body;
  auto sink = append_to(body);
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(local_request, shutdown.get_token(), cancelled, sink);

  REQUIRE(result);
  CHECK(result->status_code == 200);
  CHECK(result->provider_request_id == "final-request");
  CHECK(body == "ok");
  CHECK(result->headers.size() == 3);
  CHECK(std::ranges::none_of(result->headers, [](const HttpHeader& header) {
    return header.value == "interim-request";
  }));
}

TEST_CASE("curl transport classifies malformed and unsupported URLs locally") {
  constexpr std::array urls{
      std::string_view{"://malformed"},
      std::string_view{"unsupported-scry-scheme://localhost/"},
  };

  for (const auto url : urls) {
    CAPTURE(std::string{url});
    CurlTransport transport;
    std::string body;
    auto sink = append_to(body);
    std::stop_source shutdown;
    const std::atomic cancelled{false};

    const auto result = transport.perform(request(std::string{url}),
                                          shutdown.get_token(), cancelled, sink);

    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
    CHECK(result.error().message == "invalid server response");
    CHECK_FALSE(result.error().retryable);
    CHECK(body.empty());
  }
}

TEST_CASE("curl transport falls back to response request ID on sink failure") {
  scry::test::LoopbackServer server{
      http_response("request-id: header-request\r\n", "body")};
  CurlTransport transport;
  BodyChunkSink sink{[](std::string_view) -> scry::Status {
    return std::unexpected(scry::Error{
        .category = ErrorCategory::network,
        .message = "private message",
        .provider_detail = "provider:safe_code",
        .retryable = true,
        .retry_after = std::chrono::milliseconds{31},
        .turn_id = scry::TurnId{9},
        .attempt = 4,
    });
  }};
  std::stop_source shutdown;
  const std::atomic cancelled{false};

  const auto result =
      transport.perform(request(server.url()), shutdown.get_token(), cancelled, sink);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::network);
  CHECK(result.error().provider_detail == "provider:safe_code");
  CHECK(result.error().retryable);
  CHECK(result.error().retry_after == std::chrono::milliseconds{31});
  CHECK(result.error().turn_id == scry::TurnId{9});
  CHECK(result.error().attempt == 4);
  CHECK(result.error().provider_request_id == "header-request");
}
