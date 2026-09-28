#include "runtime/config.hpp"

#include "transport/transport_policy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <sys/un.h>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Status invalid(std::string message) {
  return std::unexpected(Error{
      .category = ErrorCategory::invalid_config,
      .message = std::move(message),
  });
}

[[nodiscard]] bool positive_limits(const ResourceLimits& limits) noexcept {
  return limits.max_pending_turns > 0 && limits.max_sse_event_bytes > 0 &&
         limits.max_response_bytes > 0 && limits.max_tool_arguments_bytes > 0 &&
         limits.max_tool_result_bytes > 0 &&
         limits.max_queued_event_bytes_per_turn > 0 &&
         limits.max_conversation_bytes > 0;
}

[[nodiscard]] bool valid_http_url(const std::string_view value) noexcept {
  constexpr auto http = std::string_view{"http://"};
  constexpr auto https = std::string_view{"https://"};
  const auto scheme_size = value.starts_with(https)
                               ? https.size()
                               : (value.starts_with(http) ? http.size() : 0);
  if (scheme_size == 0 || scheme_size == value.size()) {
    return false;
  }
  if (value.find_first_of("?# \t\r\n") != std::string_view::npos) {
    return false;
  }
  // substr clamps an npos end, so a URL with no path keeps its whole authority.
  return !value.substr(scheme_size, value.find('/', scheme_size) - scheme_size).empty();
}

// The socket path of a gRPC Unix-domain target, in either form gRPC accepts:
// `unix:` then a relative or absolute path, or `unix://` then an absolute one.
// Empty for anything else. gRPC resolves the path itself, so only what can
// never name a socket is refused here.
[[nodiscard]] std::string_view unix_socket_path(const std::string_view value) noexcept {
  constexpr auto scheme = std::string_view{"unix:"};
  constexpr auto no_authority = std::string_view{"//"};
  if (!value.starts_with(scheme)) {
    return {};
  }
  auto path = value.substr(scheme.size());
  if (path.starts_with(no_authority)) {
    path.remove_prefix(no_authority.size());
    if (!path.starts_with('/')) {
      return {};
    }
  }
  return path;
}

// sun_path holds the path and its terminator: 108 bytes on Linux, 104 on macOS.
constexpr std::size_t max_unix_socket_path = sizeof(sockaddr_un{}.sun_path) - 1;

[[nodiscard]] Status validate_unix_target(const std::string_view value) {
  constexpr auto control = std::string_view{"\0\r\n", 3};
  const auto path = unix_socket_path(value);
  if (path.empty() || path.find_first_of(control) != std::string_view::npos) {
    return invalid("llamad base_url must be a unix: gRPC target, such as "
                   "unix:/run/user/1000/llamad.sock");
  }
  if (path.size() > max_unix_socket_path) {
    return invalid("llamad socket path must fit in sockaddr_un::sun_path with its "
                   "terminator: 107 bytes on Linux, 103 on macOS");
  }
  return {};
}

// A dialect the build does not carry is refused before any of its fields are
// read, so the message names the missing build option rather than a field.
[[nodiscard]] Status validate_dialect_available(const Config& config) {
#if !SCRY_WITH_LLAMAD
  if (config.dialect == ProviderDialect::llamad) {
    return invalid("the llamad provider dialect requires a Scry build with "
                   "SCRY_WITH_LLAMAD=ON");
  }
#else
  static_cast<void>(config);
#endif
  return {};
}

[[nodiscard]] Status validate_endpoint(const Config& config) {
  if (config.dialect == ProviderDialect::llamad) {
    if (auto status = validate_unix_target(config.base_url); !status) {
      return status;
    }
  } else if (!valid_http_url(config.base_url)) {
    return invalid("base_url must be an absolute HTTP or HTTPS URL");
  }
  if (config.model.empty()) {
    return invalid("model must not be empty");
  }
  return {};
}

[[nodiscard]] Status validate_auth(const Config& config) {
  if (config.api_key.find_first_of("\r\n") != std::string::npos) {
    return invalid("api_key must contain no line breaks");
  }
  if (config.dialect == ProviderDialect::anthropic && config.api_key.empty()) {
    return invalid("Anthropic api_key must be present");
  }
  if (config.dialect == ProviderDialect::llamad && !config.api_key.empty()) {
    return invalid("llamad api_key must be empty; the daemon takes no credential");
  }
  return {};
}

[[nodiscard]] Status validate_reasoning(const Config& config) {
  switch (config.reasoning_mode) {
  case ReasoningMode::provider_default:
    return {};
  case ReasoningMode::disabled:
    if (config.dialect == ProviderDialect::openai_compatible) {
      return {};
    }
    return invalid(
        "reasoning_mode = disabled requires the OpenAI-compatible provider dialect");
  }
  return invalid("reasoning_mode is invalid");
}

[[nodiscard]] Status validate_anthropic_sampling(const SamplingConfig& sampling) {
  if (!std::isfinite(sampling.temperature) || sampling.temperature < 0.0 ||
      sampling.temperature > 1.0) {
    return invalid("Anthropic temperature must be finite and between 0 and 1");
  }
  if (sampling.top_p && (!std::isfinite(*sampling.top_p) || *sampling.top_p <= 0.0 ||
                         *sampling.top_p > 1.0)) {
    return invalid("top_p must be finite, greater than 0, and at most 1");
  }
  if (!sampling.max_tokens || *sampling.max_tokens == 0) {
    return invalid(
        "Anthropic max_tokens must be set and greater than 0; the Messages API "
        "requires it");
  }
  // The Messages API has no seed. Dropping one silently would let a host believe
  // its runs were seeded when they were not.
  if (sampling.seed) {
    return invalid("the Anthropic Messages API has no seed; use the "
                   "OpenAI-compatible or llamad dialect");
  }
  return {};
}

[[nodiscard]] Status validate_openai_sampling(const SamplingConfig& sampling) {
  if (!std::isfinite(sampling.temperature) || sampling.temperature < 0.0 ||
      sampling.temperature > 2.0) {
    return invalid("OpenAI temperature must be finite and between 0 and 2");
  }
  if (sampling.top_p && (!std::isfinite(*sampling.top_p) || *sampling.top_p < 0.0 ||
                         *sampling.top_p > 1.0)) {
    return invalid("OpenAI top_p must be finite and between 0 and 1");
  }
  // An absent max_tokens is valid for this dialect: the field is omitted from the
  // request and the server default applies.
  if (sampling.max_tokens && *sampling.max_tokens == 0) {
    return invalid("OpenAI max_tokens must be greater than 0 when set");
  }
  return {};
}

// llamad sends these as protobuf float and int32 fields. Temperature takes the
// OpenAI range, where 0 is greedy decoding, which also keeps it finite as a float.
[[nodiscard]] Status validate_llamad_sampling(const SamplingConfig& sampling) {
  if (!std::isfinite(sampling.temperature) || sampling.temperature < 0.0 ||
      sampling.temperature > 2.0) {
    return invalid("llamad temperature must be finite and between 0 and 2");
  }
  if (sampling.top_p && (!std::isfinite(*sampling.top_p) || *sampling.top_p < 0.0 ||
                         *sampling.top_p > 1.0)) {
    return invalid("llamad top_p must be finite and between 0 and 1");
  }
  constexpr auto maximum_tokens =
      static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
  if (sampling.max_tokens &&
      (*sampling.max_tokens == 0 || *sampling.max_tokens > maximum_tokens)) {
    return invalid(
        "llamad max_tokens must be greater than 0 and at most 2147483647 when set");
  }
  return {};
}

[[nodiscard]] Status validate_provider(const Config& config) {
  auto auth = validate_auth(config);
  if (!auth) {
    return auth;
  }
  auto reasoning = validate_reasoning(config);
  if (!reasoning) {
    return reasoning;
  }
  switch (config.dialect) {
  case ProviderDialect::anthropic:
    return validate_anthropic_sampling(config.sampling);
  case ProviderDialect::openai_compatible:
    return validate_openai_sampling(config.sampling);
  case ProviderDialect::llamad:
    return validate_llamad_sampling(config.sampling);
  }
  return invalid("the configured provider dialect is not available");
}

[[nodiscard]] Status validate_retry_policy(const RetryPolicy& retry) {
  if (retry.max_attempts == 0 || retry.initial_backoff.count() < 0 ||
      retry.max_backoff.count() < 0 || retry.max_elapsed.count() < 0 ||
      retry.initial_backoff > retry.max_backoff || !std::isfinite(retry.jitter_ratio) ||
      retry.jitter_ratio < 0.0 || retry.jitter_ratio > 1.0) {
    return invalid("retry policy is invalid");
  }
  return {};
}

[[nodiscard]] Status validate_runtime_bounds(const Config& config) {
  constexpr std::size_t minimum_event_bytes = 1024;
  if (auto timeouts = transport_policy::validate_timeouts(config.timeouts); !timeouts) {
    return timeouts;
  }
  if (!positive_limits(config.limits)) {
    return invalid("resource limits must be greater than 0");
  }
  if (config.limits.max_queued_event_bytes_per_turn < minimum_event_bytes) {
    return invalid("per-turn queued-event limit must be at least 1024 bytes");
  }
  if (config.max_tool_rounds == 0) {
    return invalid("max_tool_rounds must be greater than 0");
  }
  if (config.max_tool_calls_per_turn == 0) {
    return invalid("max_tool_calls_per_turn must be greater than 0 when set");
  }
  return {};
}

[[nodiscard]] bool collides_with_managed_header(const std::string_view name) noexcept {
  // Every header Scry sets itself for either dialect. A host header with one of
  // these names would either duplicate or shadow a Scry-managed value.
  constexpr std::array<std::string_view, 5> managed{
      "content-type", "accept", "authorization", "x-api-key", "anthropic-version",
  };
  return std::ranges::any_of(managed, [name](const std::string_view reserved) {
    return transport_policy::header_name_equal(name, reserved);
  });
}

// The daemon is reached over a local socket with no HTTP layer, so a value in
// any of these would be silently ignored; refusing it keeps the Config honest.
[[nodiscard]] Status validate_llamad_network_options(const Config& config) {
  if (!config.extra_headers.empty()) {
    return invalid("llamad extra_headers must be empty; the daemon takes no headers");
  }
  if (!config.proxy.empty() || !config.ca_bundle_path.empty()) {
    return invalid("llamad proxy and ca_bundle_path must be empty; the daemon is "
                   "reached over a local socket");
  }
  if (!config.tls_verify_peer) {
    return invalid("llamad tls_verify_peer must stay true; the daemon is reached "
                   "over a local socket without TLS");
  }
  return {};
}

[[nodiscard]] Status validate_network_options(const Config& config) {
  if (config.dialect == ProviderDialect::llamad) {
    return validate_llamad_network_options(config);
  }
  if (!transport_policy::validate_headers(config.extra_headers)) {
    return invalid("extra header name or value is invalid");
  }
  const auto collision =
      std::ranges::any_of(config.extra_headers, [](const HttpHeader& header) {
        return collides_with_managed_header(header.name);
      });
  if (collision) {
    return invalid("extra header collides with a Scry-managed header");
  }
  constexpr auto control = std::string_view{"\0\r\n", 3};
  constexpr auto control_and_space = std::string_view{"\0\r\n \t", 5};
  if (config.ca_bundle_path.find_first_of(control) != std::string::npos ||
      config.proxy.find_first_of(control_and_space) != std::string::npos) {
    return invalid("proxy or CA bundle path contains invalid characters");
  }
  return {};
}

} // namespace

Status validate_config(const Config& config) {
  if (auto status = validate_dialect_available(config); !status) {
    return status;
  }
  if (auto status = validate_endpoint(config); !status) {
    return status;
  }
  if (auto status = validate_provider(config); !status) {
    return status;
  }
  if (auto status = validate_retry_policy(config.retry); !status) {
    return status;
  }
  if (auto status = validate_runtime_bounds(config); !status) {
    return status;
  }
  return validate_network_options(config);
}

} // namespace scry::detail
