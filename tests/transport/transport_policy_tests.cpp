#include "transport/transport_policy.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <scry/error.hpp>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] scry::detail::BodyChunkSink sink() {
  return scry::detail::BodyChunkSink{
      [](std::string_view) -> scry::Status { return {}; }};
}

[[nodiscard]] scry::detail::TransportRequest valid_request() {
  return scry::detail::TransportRequest{
      .url = "https://example.invalid/messages",
  };
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
}

TEST_CASE("transport header policy recognizes provider correlation fields") {
  using namespace scry::detail::transport_policy;

  CHECK(header_name_equal("Content-Length", "content-length"));
  CHECK_FALSE(header_name_equal("content", "content-length"));
  CHECK_FALSE(header_name_equal("request-ia", "request-id"));
  CHECK(is_request_id_header("request-id"));
  CHECK(is_request_id_header("X-Request-ID"));
  CHECK(is_request_id_header("Anthropic-Request-Id"));
  CHECK_FALSE(is_request_id_header("trace-id"));
  CHECK(is_content_length_header("CONTENT-LENGTH"));
  CHECK_FALSE(is_content_length_header("transfer-encoding"));
}

TEST_CASE("transport size parsing rejects malformed and overflowing values") {
  using scry::detail::transport_policy::parse_size;

  CHECK(parse_size("42") == 42);
  CHECK_FALSE(parse_size(""));
  CHECK_FALSE(parse_size("12bytes"));
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
  CHECK(http_error(403, "").category == ErrorCategory::authentication);

  const auto rate_limit = http_error(429, "");
  CHECK(rate_limit.category == ErrorCategory::rate_limit);
  CHECK(rate_limit.retryable);

  const auto server_error = http_error(599, "");
  CHECK(server_error.category == ErrorCategory::network);
  CHECK(server_error.retryable);

  const auto redirect = http_error(301, "");
  CHECK(redirect.category == ErrorCategory::protocol);
  CHECK_FALSE(redirect.retryable);
}

TEST_CASE("response policy parses status, headers, and bounded body bytes") {
  using scry::detail::transport_policy::ResponseState;

  ResponseState response{.limit = 512};
  REQUIRE(response.accept_header("HTTP/1.1 200 OK\r\n"));
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
  REQUIRE(redirect.accept_header("HTTP/1.1 204 No Content\r\n"));
  CHECK(redirect.deliver_body);
}

TEST_CASE("response policy rejects malformed and oversized metadata") {
  using scry::ErrorCategory;
  using scry::detail::transport_policy::ResponseState;

  for (const auto line : {"HTTP/1.1", "HTTP/1.1 nope Bad"}) {
    ResponseState response{.limit = 512};
    const auto result = response.accept_header(line);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }

  for (const auto line :
       {"missing-separator\r\n", " : value\r\n", "Content-Length: invalid\r\n"}) {
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

  ResponseState bounded{.limit = 4};
  REQUIRE(bounded.account_body(4));
  result = bounded.account_body(1);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::resource_limit);
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
  CHECK(sanitize_provider_detail("safe:" + std::string(128, 'x')).empty());
}
