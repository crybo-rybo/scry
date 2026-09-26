#include "transport/curl_error.hpp"
#include "transport/transport_policy.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <curl/curl.h>
#include <limits>
#include <optional>
#include <scry/error.hpp>
#include <scry/turn_id.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

using scry::ErrorCategory;
using scry::HttpHeader;

void check_category(const scry::Error& error, const ErrorCategory category,
                    const bool retryable, const char* message) {
  CHECK(error.category == category);
  CHECK(error.retryable == retryable);
  CHECK(error.message == message);
}

} // namespace

TEST_CASE("transport request validation rejects incomplete requests") {
  using scry::detail::transport_policy::validate_request;

  scry::detail::BodyChunkSink body_sink{
      [](std::string_view) -> scry::Status { return {}; }};
  auto request = scry::detail::TransportRequest{
      .url = "https://example.invalid/messages",
  };
  CHECK(validate_request(request, body_sink));

  scry::detail::BodyChunkSink missing_sink;
  auto result = validate_request(request, missing_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_state);

  request.timeouts.idle = std::chrono::milliseconds::zero();
  result = validate_request(request, body_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_config);

  request.url.clear();
  result = validate_request(request, body_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_config);
  CHECK(result.error().message == "transport URL is empty");
}

TEST_CASE("transport timeout validation requires positive bounds") {
  using namespace std::chrono_literals;
  using scry::detail::transport_policy::validate_timeouts;

  // An unset total transfer bound is the default and stays accepted; a set one
  // must still be positive.
  CHECK(validate_timeouts({}));
  CHECK(validate_timeouts({.transfer = 1ms}));
  for (const auto& timeouts : {
           scry::TransportTimeouts{.connect = 0ms},
           scry::TransportTimeouts{.idle = 0ms},
           scry::TransportTimeouts{.transfer = 0ms},
           scry::TransportTimeouts{.transfer = -1ms},
           scry::TransportTimeouts{.shutdown = 0ms},
       }) {
    const auto result = validate_timeouts(timeouts);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::invalid_config);
  }
}

TEST_CASE("transport header validation accepts RFC tokens and rejects injection") {
  using scry::detail::transport_policy::validate_headers;

  CHECK(validate_headers({}));
  CHECK(validate_headers({HttpHeader{.name = "x!#$%&'*+-.^_`|~", .value = "safe"}}));

  auto result = validate_headers({HttpHeader{.name = "", .value = "safe"}});
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::protocol);

  result = validate_headers({HttpHeader{.name = "bad header", .value = "safe"}});
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::protocol);

  result = validate_headers({HttpHeader{.name = "x-safe", .value = "bad\r\n"}});
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::protocol);

  // A value is an RFC 9110 field-value: every control byte but tab is rejected,
  // because curl_slist_append takes a C string and an embedded NUL would send a
  // silently truncated header instead of failing here.
  for (const char control : {'\0', '\x01', '\x1f', '\x7f'}) {
    result = validate_headers(
        {HttpHeader{.name = "x-safe", .value = std::string{"a"} + control + "b"}});
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }
  CHECK(validate_headers({HttpHeader{.name = "x-safe", .value = "a\tb"}}));
  // obs-text stays legal, so a UTF-8 value still passes.
  CHECK(validate_headers({HttpHeader{.name = "x-safe", .value = "caf\xc3\xa9"}}));
}

TEST_CASE("transport header names compare case-insensitively") {
  using scry::detail::transport_policy::header_name_equal;

  CHECK(header_name_equal("Content-Length", "content-length"));
  CHECK_FALSE(header_name_equal("content", "content-length"));
  CHECK_FALSE(header_name_equal("request-ia", "request-id"));
  CHECK_FALSE(header_name_equal("request-ix", "request-id"));
}

TEST_CASE("transport size parsing rejects malformed and overflowing values") {
  using scry::detail::transport_policy::parse_size;

  CHECK(parse_size("42") == 42);
  CHECK(parse_size(std::to_string(std::numeric_limits<std::size_t>::max())) ==
        std::numeric_limits<std::size_t>::max());
  CHECK_FALSE(parse_size(""));
  CHECK_FALSE(parse_size("12bytes"));
  CHECK_FALSE(parse_size("+1"));
  CHECK_FALSE(parse_size(" 1"));
  CHECK_FALSE(
      parse_size(std::to_string(std::numeric_limits<std::size_t>::max()) + "0"));
}

TEST_CASE("HTTP status policy maps retryability and preserves correlation") {
  using scry::detail::transport_policy::http_error;

  struct Case {
    std::int32_t status;
    ErrorCategory category;
    bool retryable;
  };
  // Only the 5xx range is treated as a retryable server failure.
  constexpr std::array cases{
      Case{401, ErrorCategory::authentication, false},
      Case{403, ErrorCategory::authentication, false},
      Case{429, ErrorCategory::rate_limit, true},
      Case{301, ErrorCategory::protocol, false},
      Case{499, ErrorCategory::protocol, false},
      Case{500, ErrorCategory::network, true},
      Case{599, ErrorCategory::network, true},
      Case{600, ErrorCategory::protocol, false},
  };

  for (const auto& test_case : cases) {
    CAPTURE(test_case.status);
    const auto error = http_error(test_case.status, "request-1");
    CHECK(error.category == test_case.category);
    CHECK(error.retryable == test_case.retryable);
    CHECK(error.http_status == test_case.status);
    CHECK(error.provider_request_id == "request-1");
  }
}

TEST_CASE("HTTP error detail extracts only a sanitized provider token") {
  using scry::detail::transport_policy::http_error_detail;

  CHECK(http_error_detail(
            R"({"type":"error","error":{"type":"not_found_error","message":"secret"}})",
            "anthropic") == "anthropic:not_found_error");
  CHECK(
      http_error_detail(
          R"({"error":{"message":"secret","type":"invalid_request_error","code":"model_not_found"}})",
          "openai") == "openai:invalid_request_error");
  CHECK(http_error_detail(R"({"error":{"message":"secret","code":"model_not_found"}})",
                          "openai") == "openai:model_not_found");

  // The body never reaches the detail: anything that is not a clean token in an
  // error object collapses to empty rather than to a placeholder.
  CHECK(http_error_detail("not json at all", "anthropic").empty());
  // A truncated error body is a broken response; empty detail is the safe
  // default, whether the cut lands after a complete token or mid-token.
  CHECK(
      http_error_detail(R"({"error":{"type":"not_found_error")", "anthropic").empty());
  CHECK(http_error_detail(R"({"error":{"type":"not_fou)", "anthropic").empty());
  CHECK(http_error_detail(R"({"error":{"type":"not found"}})", "anthropic").empty());
  CHECK(http_error_detail(R"({"error":{"type":"not-found-error"}})", "anthropic")
            .empty());
  CHECK(http_error_detail(R"({"error":{"type":"not_found_error"}})", "").empty());
  CHECK(http_error_detail(R"({"error":"not_found_error"})", "anthropic").empty());
  CHECK(http_error_detail(R"({"type":"not_found_error"})", "anthropic").empty());
  CHECK(http_error_detail("", "anthropic").empty());
}

TEST_CASE("a retained error body stops growing at the documented cap") {
  using scry::detail::transport_policy::append_error_body;
  using scry::detail::transport_policy::maximum_error_body_bytes;

  std::string body;
  append_error_body(body, std::string(maximum_error_body_bytes - 1, 'a'));
  append_error_body(body, "bc");
  CHECK(body.size() == maximum_error_body_bytes);
  CHECK(body.back() == 'b');
  append_error_body(body, "d");
  CHECK(body.size() == maximum_error_body_bytes);
}

TEST_CASE("response policy parses status, headers, and bounded body bytes") {
  using scry::detail::transport_policy::ResponseState;

  ResponseState response{.limit = 512};
  CHECK(response.status_code == 0);
  REQUIRE(response.accept_header("HTTP/1.1 200 OK\r\n"));
  CHECK(response.status_code == 200);
  REQUIRE(response.accept_header(" Request-Id : request-42 \r\n"));
  REQUIRE(response.accept_header("Content-Length: 4\r\n"));
  REQUIRE(response.accept_header("\r\n"));
  CHECK(response.deliver_body);
  CHECK(response.provider_request_id == "request-42");
  REQUIRE(response.account_body(4));
  CHECK(response.received_bytes > 4);

  // Every provider spelling of the correlation header is recognized, and an
  // unrelated one is not.
  REQUIRE(response.accept_header("Anthropic-Request-Id: request-43\r\n"));
  REQUIRE(response.accept_header("trace-id: not-a-request-id\r\n"));
  CHECK(response.provider_request_id == "request-43");

  ResponseState redirect{.limit = 128};
  REQUIRE(redirect.accept_header("HTTP/1.1 302 Found\r\n"));
  CHECK_FALSE(redirect.deliver_body);
  CHECK(redirect.status_code == 302);
  REQUIRE(redirect.accept_header("HTTP/1.1 204 No Content\r\n"));
  CHECK(redirect.deliver_body);
  CHECK(redirect.status_code == 204);
}

TEST_CASE("response policy rejects malformed and oversized metadata") {
  using scry::detail::transport_policy::ResponseState;

  for (const auto line :
       {"HTTP/1.1", "HTTP/1.1 ", "HTTP/1.1 nope Bad", "HTTP/1.1 200x OK"}) {
    CAPTURE(line);
    ResponseState response{.limit = 512};
    const auto result = response.accept_header(line);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }

  for (const auto line :
       {"missing-separator\r\n", " : value\r\n", "Content-Length: invalid\r\n"}) {
    CAPTURE(line);
    ResponseState response{.limit = 512};
    const auto result = response.accept_header(line);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }

  ResponseState declared{.limit = 64};
  auto result = declared.accept_header("Content-Length: 65");
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);

  ResponseState identifier{.limit = 1024};
  result = identifier.accept_header("Request-Id: " + std::string(257, 'x'));
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::protocol);

  // Header bytes count against the same budget as the body.
  ResponseState metadata{.limit = 3};
  result = metadata.accept_header("four");
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);

  ResponseState bounded{.limit = 4};
  REQUIRE(bounded.account_body(4));
  result = bounded.account_body(1);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);

  ResponseState overrun{.limit = 8, .received_bytes = 6};
  result = overrun.account_body(3);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);

  // A tally already past the limit stays rejected even for an empty chunk.
  ResponseState corrupt{.limit = 4, .received_bytes = 5};
  result = corrupt.account_body(0);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);
}

TEST_CASE("response policy resets metadata when a new status line arrives") {
  using scry::detail::transport_policy::ResponseState;

  ResponseState response{.limit = 2048};
  REQUIRE(response.accept_header("HTTP/2 204"));
  CHECK(response.deliver_body);

  const std::string identifier(256, 'r');
  REQUIRE(response.accept_header(" X-ReQuEsT-Id : " + identifier + " \r\n"));
  REQUIRE(response.accept_header("Content-Length: 2048\r\n"));
  REQUIRE(response.accept_header("ReTrY-AfTeR: 7\r\n"));
  CHECK(response.provider_request_id == identifier);
  // Only the consumed headers are retained: the request id and every
  // Retry-After value. Content-Length is validated and dropped.
  REQUIRE(response.retry_after_values == std::vector<std::string>{"7"});

  REQUIRE(response.accept_header("HTTP/1.1 101 Switching Protocols\r\n"));
  CHECK_FALSE(response.deliver_body);
  CHECK(response.retry_after_values.empty());
  CHECK(response.provider_request_id.empty());
  REQUIRE(response.accept_header(" \t\r\n"));
}

TEST_CASE("provider detail policy accepts only bounded structured identifiers") {
  using scry::detail::transport_policy::sanitize_provider_detail;

  CHECK(sanitize_provider_detail("anthropic:overloaded_error") ==
        "anthropic:overloaded_error");
  CHECK(sanitize_provider_detail("provider_2:error42") == "provider_2:error42");
  CHECK(sanitize_provider_detail("").empty());
  CHECK(sanitize_provider_detail("missing_namespace").empty());
  CHECK(sanitize_provider_detail(":missing").empty());
  CHECK(sanitize_provider_detail("missing:").empty());
  CHECK(sanitize_provider_detail("too:many:parts").empty());
  CHECK(sanitize_provider_detail("unsafe:value-with-content").empty());
  CHECK(sanitize_provider_detail("p:unsafe value").empty());
  CHECK(sanitize_provider_detail("safe:" + std::string(128, 'x')).empty());

  // The longest identifier that still fits survives unchanged; one byte more
  // is dropped entirely.
  const std::string maximum_detail = "p:" + std::string(126, 'x');
  CHECK(sanitize_provider_detail(maximum_detail) == maximum_detail);
  CHECK(sanitize_provider_detail("p:" + std::string(127, 'x')).empty());
}

TEST_CASE("curl error classification prefers consumer errors and cancellation causes") {
  using scry::detail::curl_error::AbortCause;
  using scry::detail::curl_error::classify;

  const scry::Error callback{
      .category = ErrorCategory::tool,
      .retryable = true,
      .attempt = 3,
      .message = "consumer failed",
      .provider_detail = "provider:safe_code",
      .retry_after = std::chrono::milliseconds{17},
      .turn_id = scry::TurnId{42},
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

TEST_CASE("curl error classification maps transfer codes onto error categories") {
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
      Case{CURLE_OPERATION_TIMEDOUT, ErrorCategory::network, "transfer timed out",
           true},
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

  const auto fallback = retry_after({"not-a-date", "7"});
  REQUIRE(fallback);
  CHECK(*fallback == std::chrono::seconds{7});
  CHECK(retry_after({"0"}) == std::chrono::milliseconds::zero());

  const auto saturated =
      retry_after({std::to_string(std::numeric_limits<std::size_t>::max())});
  REQUIRE(saturated);
  constexpr auto maximum = std::numeric_limits<std::chrono::milliseconds::rep>::max();
  CHECK(saturated->count() == (maximum / 1000) * 1000);

  CHECK_FALSE(retry_after({}));
  CHECK_FALSE(retry_after({"still-not-a-date"}));
}

TEST_CASE("Retry-After clamps past dates and accepts future HTTP dates") {
  using scry::detail::curl_error::retry_after;

  const auto past = retry_after({"Thu, 01 Jan 1970 00:00:00 GMT"});
  REQUIRE(past);
  CHECK(*past == std::chrono::milliseconds::zero());

  const auto future = retry_after({"Wed, 21 Oct 2099 07:28:00 GMT"});
  REQUIRE(future);
  CHECK(*future > std::chrono::hours{24});
}
