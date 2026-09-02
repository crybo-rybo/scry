/// @file
/// @brief Curl-independent HTTP validation, accounting, and error policy.
///
/// Untrusted request/response metadata is checked here before it reaches curl
/// or provider decoders. Keeping policy independent of curl makes hostile-input
/// and byte-limit behavior deterministic and directly testable.

#pragma once

#include "core/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace scry::detail::transport_policy {

/// @brief Mutable accounting state for one HTTP response header/body stream.
///
/// Redirect/intermediate status lines reset final-header metadata. All received
/// header and body bytes share the same configured response ceiling; body bytes
/// are delivered only for a successful 2xx status.
struct ResponseState {
  std::size_t limit{};               ///< Maximum cumulative response bytes.
  std::size_t received_bytes{};      ///< Header and body bytes accepted so far.
  std::vector<HttpHeader> headers{}; ///< Current/final response header block.
  std::string provider_request_id{}; ///< Bounded recognized request ID.
  bool deliver_body{false}; ///< Whether final status currently permits body delivery.

  /// @brief Validates, accounts, and records one raw response-header line.
  /// @param line Raw curl header bytes; surrounding whitespace is ignored.
  /// @return Success, or a protocol/resource error before unsafe retention.
  [[nodiscard]] Status accept_header(std::string_view line);
  /// @brief Reserves response budget for an incoming body chunk.
  /// @param bytes Chunk size to add.
  /// @return Success, or `resource_limit` without changing the counter.
  [[nodiscard]] Status account_body(std::size_t bytes);
};

/// @brief Compares HTTP field names case-insensitively as ASCII bytes.
/// @param left First field name.
/// @param right Second field name.
/// @return Whether both names have equal length and case-folded bytes.
[[nodiscard]] bool header_name_equal(std::string_view left,
                                     std::string_view right) noexcept;

/// @brief Recognizes supported provider correlation header names.
/// @param name Candidate HTTP field name.
/// @return Whether Scry treats the field as a provider request identifier.
[[nodiscard]] bool is_request_id_header(std::string_view name) noexcept;

/// @brief Recognizes the HTTP `Content-Length` field name.
/// @param name Candidate HTTP field name.
/// @return Whether `name` is `Content-Length`, ignoring ASCII case.
[[nodiscard]] bool is_content_length_header(std::string_view name) noexcept;

/// @brief Parses a complete nonnegative decimal size without whitespace.
/// @param value Candidate decimal text.
/// @return Parsed value, or `nullopt` for invalid text or overflow.
[[nodiscard]] std::optional<std::size_t> parse_size(std::string_view value) noexcept;

/// @brief Validates preconditions required by every transport operation.
/// @param request Request whose endpoint and timeout bounds are checked.
/// @param body_sink Response consumer, which must be nonempty.
/// @return Success or `invalid_config`/`invalid_state`.
[[nodiscard]] Status validate_request(const TransportRequest& request,
                                      const BodyChunkSink& body_sink);

/// @brief Rejects unsafe outgoing header names and line injection.
/// @param headers Provider-generated request headers.
/// @return Success, or a protocol error before curl receives the headers.
[[nodiscard]] Status validate_headers(const std::vector<HttpHeader>& headers);

/// @brief Classifies a non-success HTTP response without copying its body.
/// @param status Final HTTP status code.
/// @param request_id Bounded provider correlation identifier from response headers.
/// @return Authentication, rate-limit, retryable server, or protocol error.
[[nodiscard]] Error http_error(std::int32_t status, const std::string& request_id);

/// @brief Sanitizes a structured provider diagnostic token.
///
/// Only an at-most-128-byte `namespace:token` spelling made of alphanumeric,
/// underscore, and the single separator survives. Arbitrary provider prose,
/// credentials, and message content are discarded.
///
/// @param detail Untrusted provider-controlled detail.
/// @return Safe owning detail or an empty string.
[[nodiscard]] std::string sanitize_provider_detail(std::string_view detail);

} // namespace scry::detail::transport_policy
