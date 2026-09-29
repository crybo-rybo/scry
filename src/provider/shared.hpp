#pragma once

// Small helpers both dialect adapters need. The two stream state machines stay
// independent; only the pieces that are byte-for-byte identical live here.
//
// Request wire structs. Each request encoder describes its body as reflected
// aggregates and encodes them with the reflected codec, which writes keys in
// canonical order whatever the declaration order and strings with the canonical
// escapes. The structs borrow every byte they write: text through
// std::string_view, and stored JSON payloads through `const Json&` members, which
// the codec validates with one allocation-free scan and splices verbatim. The
// borrowed Config, ModelRequest, and literals all outlive the encode.

#include "core/provider.hpp"
#include "kernel/error.hpp"
#include "kernel/json/codec.hpp"
#include "reflection/codec.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <scry/config.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {

// Calls `encode` for the committed history, then for the turn's own messages,
// stopping at the first failure.
template <class Encode>
[[nodiscard]] Status for_each_request_message(const ModelRequest& request,
                                              Encode&& encode) {
  if (request.history) {
    for (const auto& message : *request.history) {
      if (auto status = encode(message); !status) {
        return status;
      }
    }
  }
  for (const auto& message : request.messages) {
    if (auto status = encode(message); !status) {
      return status;
    }
  }
  return {};
}

[[nodiscard]] inline std::string trim_trailing_slashes(std::string url) {
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

// Config is immutable per Harness and validated once at Harness::create, so the
// adapters build the request without re-checking endpoint, auth, or sampling
// bounds. The host's verbatim extra headers follow the dialect's own; validation
// has already rejected any name that collides with a Scry-managed header, so the
// order is presentation only.
[[nodiscard]] inline TransportRequest
transport_request(const Config& config, std::string url,
                  std::vector<HttpHeader> headers, std::string body,
                  const std::string_view provider_namespace) {
  headers.insert(headers.end(), config.extra_headers.begin(),
                 config.extra_headers.end());
  return TransportRequest{
      .url = std::move(url),
      .headers = std::move(headers),
      .body = std::move(body),
      .provider_namespace = std::string{provider_namespace},
      .tls_verify_peer = config.tls_verify_peer,
      .ca_bundle_path = config.ca_bundle_path,
      .proxy = config.proxy,
      .timeouts = config.timeouts,
      .limits = config.limits,
  };
}

// Every JSON payload a request encoder embeds is canonical text Scry's own codec
// produced, so the encoders use the stored text after one allocation-free
// validation scan of every byte instead of re-parsing it into a document. That
// scan keeps the rejection contract for a ModelRequest assembled by hand. A
// payload spliced through a `const Json&` member is scanned by the codec as it is
// written, so before the encode only its root is checked here; a payload carried
// as a JSON string is scanned here, since the codec only quotes it.
[[nodiscard]] inline Status embedded_object_root(const std::string_view text,
                                                 const std::string_view message) {
  const auto first = text.find_first_not_of(" \t\n\r");
  if (first == std::string_view::npos || text[first] != '{') {
    return std::unexpected(
        make_error(ErrorCategory::invalid_config, std::string{message}));
  }
  return {};
}

[[nodiscard]] inline Status embedded_json_object(const std::string_view text,
                                                 const std::string_view message) {
  return validate_json_object(text, ErrorCategory::invalid_config, message);
}

[[nodiscard]] inline Status embedded_json_value(const std::string_view text,
                                                const std::string_view message) {
  return validate_json(text, ErrorCategory::invalid_config, message);
}

// Writes a request body. Sampling numbers reach the body as canonical_json_number
// text, because the codec spells a double in std::to_chars' shortest form and the
// body is not canonicalized afterwards. A failure is a spliced payload that is not
// JSON, reported with its path.
template <typename Body>
[[nodiscard]] Result<std::string> encode_request_body(const Body& body,
                                                      const std::string_view dialect) {
  auto encoded = encode_text(body);
  if (!encoded) {
    return std::unexpected(
        make_error(ErrorCategory::invalid_config,
                   std::string{dialect} + " request at " + describe(encoded.error())));
  }
  return std::move(*encoded);
}

// Provider error identifiers reach Error::provider_detail, so only a bounded token
// of alphanumerics and underscores survives; anything else is dropped.
[[nodiscard]] inline std::optional<std::string>
sanitize_error_token(const std::string_view value) {
  constexpr std::size_t maximum_bytes = 96;
  if (value.empty() || value.size() > maximum_bytes) {
    return std::nullopt;
  }
  const auto safe = std::ranges::all_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0 || character == '_';
  });
  return safe ? std::optional<std::string>{value} : std::nullopt;
}

// The sanitized string at `root.error.<field>`, or nullopt when it is absent,
// not a string, or unsafe.
[[nodiscard]] inline std::optional<std::string>
error_token(const JsonValue& root, const std::string_view field) {
  const auto* error = json_field(root, "error");
  const auto* value = error == nullptr ? nullptr : json_field(*error, field);
  if (value == nullptr || !value->is_string()) {
    return std::nullopt;
  }
  return sanitize_error_token(value->get_string());
}

// The error types both dialects share. A dialect with its own aliases checks
// those first and falls back to this.
[[nodiscard]] inline ErrorCategory
error_category(const std::string_view token) noexcept {
  if (token == "authentication_error" || token == "permission_error") {
    return ErrorCategory::authentication;
  }
  if (token == "rate_limit_error") {
    return ErrorCategory::rate_limit;
  }
  if (token == "overloaded_error" || token == "api_error") {
    return ErrorCategory::network;
  }
  return ErrorCategory::protocol;
}

// A provider-reported stream error: rate limits and server-side failures are
// retryable, and the detail carries the namespaced, already-sanitized token.
[[nodiscard]] inline Error provider_error(const ErrorCategory category,
                                          const std::string_view message,
                                          std::string detail) {
  const auto retryable =
      category == ErrorCategory::rate_limit || category == ErrorCategory::network;
  Error error = make_error(category, std::string{message}, retryable);
  error.provider_detail = std::move(detail);
  return error;
}

// Reads a required non-negative "index" that fits a std::size_t.
[[nodiscard]] inline Result<std::size_t>
required_index(const JsonValue& value, const std::string_view message) {
  auto parsed = optional_json_uint(value, "index");
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  const auto index = *parsed;
  if (!index ||
      *index > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(make_error(ErrorCategory::protocol, std::string{message}));
  }
  return static_cast<std::size_t>(*index);
}

// Claims the shared decode state for one dialect. The terminal event moves the
// accumulated response out of the decode state, so every later event is refused
// before anything reads it again; a state another dialect already owns is
// refused too.
template <typename Dialect>
[[nodiscard]] Result<Dialect*> begin_event(ProviderDecodeState& state,
                                           const std::string_view dialect_name) {
  if (state.completed) {
    return std::unexpected(
        make_error(ErrorCategory::protocol, std::string{dialect_name} +
                                                " stream emitted data after its "
                                                "terminal event"));
  }
  if (std::holds_alternative<std::monostate>(state.dialect)) {
    state.dialect.template emplace<Dialect>();
  }
  auto* decode = std::get_if<Dialect>(&state.dialect);
  if (decode == nullptr) {
    return std::unexpected(make_error(
        ErrorCategory::protocol, std::string{dialect_name} +
                                     " stream received decode state owned by another "
                                     "dialect"));
  }
  return decode;
}

// begin_event rejects every later event once completed is set, so the
// accumulated response has no reader left and transfers to the terminal event.
inline void complete_response(ProviderDecodeState& state,
                              std::vector<ProviderEvent>& out) {
  state.completed = true;
  out.push_back(ProviderCompleted{.response = std::move(state.response)});
}

// Appends a tool-argument fragment, rejecting it first when it would grow the
// destination past the configured budget.
[[nodiscard]] inline Status append_tool_arguments(std::string& destination,
                                                  const std::string_view fragment,
                                                  const std::size_t limit,
                                                  const std::string_view message) {
  if (destination.size() > limit || fragment.size() > limit - destination.size()) {
    return std::unexpected(
        make_error(ErrorCategory::resource_limit, std::string{message}));
  }
  destination.append(fragment);
  return {};
}

} // namespace scry::detail
