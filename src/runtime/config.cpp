#include "runtime/config.hpp"

#include <cmath>
#include <string>
#include <string_view>
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
  if (value.find_first_of("?#") != std::string_view::npos) {
    return false;
  }
  const auto authority_end = value.find_first_of("/?#", scheme_size);
  const auto authority =
      value.substr(scheme_size, authority_end == std::string_view::npos
                                    ? std::string_view::npos
                                    : authority_end - scheme_size);
  return !authority.empty() && value.find_first_of(" \t\r\n") == std::string_view::npos;
}

[[nodiscard]] Status validate_endpoint(const Config& config) {
  if (!valid_http_url(config.base_url)) {
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
  return {};
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
    return invalid("Anthropic max_tokens must be configured and greater than 0");
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
  if (!sampling.max_tokens || *sampling.max_tokens == 0) {
    return invalid("OpenAI max_tokens must be configured and greater than 0");
  }
  return {};
}

[[nodiscard]] Status validate_provider(const Config& config) {
  auto auth = validate_auth(config);
  if (!auth) {
    return auth;
  }
  switch (config.dialect) {
  case ProviderDialect::anthropic:
    return validate_anthropic_sampling(config.sampling);
  case ProviderDialect::openai_compatible:
    return validate_openai_sampling(config.sampling);
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
  if (config.timeouts.connect.count() <= 0 || config.timeouts.transfer.count() <= 0 ||
      config.timeouts.shutdown.count() <= 0) {
    return invalid("transport timeouts must be greater than 0");
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
  return {};
}

} // namespace

Status validate_config(const Config& config) {
  if (auto status = validate_endpoint(config); !status) {
    return status;
  }
  if (auto status = validate_provider(config); !status) {
    return status;
  }
  if (auto status = validate_retry_policy(config.retry); !status) {
    return status;
  }
  return validate_runtime_bounds(config);
}

} // namespace scry::detail
