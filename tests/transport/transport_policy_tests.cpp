#include "transport/curl_error.hpp"
#include "transport/transport_policy.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <curl/curl.h>
#include <limits>
#include <optional>
#include <scry/error.hpp>
#include <scry/turn_id.hpp>
#include <string>
#include <string_view>

namespace {

using scry::ErrorCategory;
using scry::detail::HttpHeader;

[[nodiscard]] scry::detail::BodyChunkSink sink() {
  return scry::detail::BodyChunkSink{
      [](std::string_view) -> scry::Status { return {}; }};
}

[[nodiscard]] scry::detail::TransportRequest valid_request() {
  return scry::detail::TransportRequest{
      .url = "https://example.invalid/messages",
  };
}

void check_category(const scry::Error& error, const ErrorCategory category,
                    const bool retryable, const char* message) {
  CHECK(error.category == category);
  CHECK(error.retryable == retryable);
  CHECK(error.message == message);
}

} // namespace

TEST_CASE("transport request validation rejects incomplete requests") {
  using namespace std::chrono_literals;
  using scry::ErrorCategory;
  using scry::detail::transport_policy::validate_request;

  auto body_sink = sink();
  auto request = valid_request();
  CHECK(validate_request(request, body_sink));

  request.url.clear();
  auto result = validate_request(request, body_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_config);

  request = valid_request();
  scry::detail::BodyChunkSink missing_sink;
  result = validate_request(request, missing_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_state);

  request.timeouts.connect = 0ms;
  result = validate_request(request, body_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_config);

  request = valid_request();
  request.timeouts.idle = 0ms;
  result = validate_request(request, body_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_config);

  // An unset total transfer bound is the default and stays accepted; a set one
  // must still be positive.
  request = valid_request();
  request.timeouts.transfer = {};
  CHECK(validate_request(request, body_sink));

  request.timeouts.transfer = 0ms;
  result = validate_request(request, body_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_config);

  request = valid_request();
  request.timeouts.transfer = -1ms;
  result = validate_request(request, body_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_config);

  request = valid_request();
  request.timeouts.shutdown = 0ms;
  result = validate_request(request, body_sink);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_config);
}

TEST_CASE("transport header validation accepts RFC tokens and rejects injection") {
  using scry::ErrorCategory;
  using scry::detail::HttpHeader;
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

TEST_CASE("transport header policy recognizes provider correlation fields") {
  using namespace scry::detail::transport_policy;

  CHECK(header_name_equal("Content-Length", "content-length"));
  CHECK_FALSE(header_name_equal("content", "content-length"));
  CHECK_FALSE(header_name_equal("request-ia", "request-id"));
  CHECK_FALSE(header_name_equal("request-ix", "request-id"));
  CHECK(is_request_id_header("request-id"));
  CHECK(is_request_id_header("x-request-id"));
  CHECK(is_request_id_header("X-Request-ID"));
  CHECK(is_request_id_header("anthropic-request-id"));
  CHECK(is_request_id_header("Anthropic-Request-Id"));
  CHECK_FALSE(is_request_id_header("trace-id"));
  CHECK(is_content_length_header("CONTENT-LENGTH"));
  CHECK_FALSE(is_content_length_header("transfer-encoding"));
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
  using scry::ErrorCategory;
  using scry::detail::transport_policy::http_error;

  const auto unauthorized = http_error(401, "request-1");
  CHECK(unauthorized.category == ErrorCategory::authentication);
  CHECK_FALSE(unauthorized.retryable);
  CHECK(unauthorized.provider_request_id == "request-1");
  CHECK(unauthorized.http_status == 401);
  CHECK(http_error(403, "").category == ErrorCategory::authentication);
  CHECK(http_error(403, "").http_status == 403);

  const auto rate_limit = http_error(429, "");
  CHECK(rate_limit.category == ErrorCategory::rate_limit);
  CHECK(rate_limit.retryable);
  CHECK(rate_limit.http_status == 429);

  const auto server_error = http_error(599, "");
  CHECK(server_error.category == ErrorCategory::network);
  CHECK(server_error.retryable);
  CHECK(server_error.http_status == 599);

  const auto redirect = http_error(301, "");
  CHECK(redirect.category == ErrorCategory::protocol);
  CHECK_FALSE(redirect.retryable);
  CHECK(redirect.http_status == 301);

  // Only the 5xx range is treated as a retryable server failure.
  CHECK(http_error(499, "").category == ErrorCategory::protocol);
  CHECK(http_error(500, "").category == ErrorCategory::network);
  CHECK(http_error(600, "").category == ErrorCategory::protocol);
  CHECK(http_error(499, "").http_status == 499);
  CHECK(http_error(500, "").http_status == 500);
  CHECK(http_error(600, "").http_status == 600);
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
  CHECK(response.provider_request_id == identifier);
  REQUIRE(response.headers.size() == 2);

  REQUIRE(response.accept_header("HTTP/1.1 101 Switching Protocols\r\n"));
  CHECK_FALSE(response.deliver_body);
  CHECK(response.headers.empty());
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
