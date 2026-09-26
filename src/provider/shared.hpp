#pragma once

// Small helpers both dialect adapters need. The two stream state machines stay
// independent; only the pieces that are byte-for-byte identical live here.
//
// Request wire structs. Each request encoder describes its body as plain
// aggregates instead of a JSON tree. Glaze reflects each one member by member
// in declaration order, so the members are declared alphabetically and the body
// still leaves the encoder in the codec's canonical key order. Every
// std::string_view borrows from the Config or the ModelRequest, both of which
// outlive the encode, and every JsonText splices stored canonical JSON verbatim
// rather than re-parsing it. The names are dialect-qualified and the types
// deliberately sit outside the unnamed namespace: Glaze derives each member's
// name from a pointer into an `extern` object of the type, which a type with no
// linkage cannot have, and GCC mangles every translation unit's unnamed
// namespace identically, so two same-named wire structs in the two request files
// would have their key tables merged by the linker and each dialect would
// serialize with the other's keys.

#include "core/error.hpp"
#include "core/json_codec.hpp"
#include "core/provider.hpp"

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
// produced, so the encoders splice the stored text after one allocation-free
// validation scan of every byte instead of re-parsing it into a document. That
// scan keeps the rejection contract for a ModelRequest assembled by hand.
[[nodiscard]] inline Status embedded_json_object(const std::string_view text,
                                                 const std::string_view message) {
  return validate_json_object(text, ErrorCategory::invalid_config, message);
}

[[nodiscard]] inline Status embedded_json_value(const std::string_view text,
                                                const std::string_view message) {
  return validate_json(text, ErrorCategory::invalid_config, message);
}

// Provider error identifiers reach Error::provider_detail, so only a bounded
// alphanumeric token survives; anything else is dropped.
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
