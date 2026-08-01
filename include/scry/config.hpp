#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

/// Stable C++23 API for the Scry runtime.
namespace scry {

/// Selects the wire protocol used by a Harness.
enum class ProviderDialect : std::uint8_t {
  /// Anthropic Messages API.
  anthropic,
  /// The supported OpenAI-compatible Chat Completions subset.
  openai_compatible,
};

/// Sampling parameters sent with each model request.
///
/// Values are validated by Harness::create() for the selected provider dialect.
struct SamplingConfig {
  /// Sampling temperature.
  double temperature{1.0};
  /// Optional nucleus-sampling probability.
  std::optional<double> top_p{};
  /// Optional maximum number of output tokens requested from the provider.
  std::optional<std::uint32_t> max_tokens{1024};
};

/// Retry limits and exponential-backoff settings for transient failures.
struct RetryPolicy {
  /// Maximum number of attempts for one model request, including the first attempt.
  std::uint32_t max_attempts{3};
  /// Backoff before the second attempt.
  std::chrono::milliseconds initial_backoff{250};
  /// Maximum backoff between attempts.
  std::chrono::milliseconds max_backoff{10'000};
  /// Maximum elapsed retry window for one model request.
  std::chrono::milliseconds max_elapsed{30'000};
  /// Fractional random variation applied to calculated backoffs.
  double jitter_ratio{0.2};
};

/// Time bounds for Scry-owned network and shutdown operations.
struct TransportTimeouts {
  /// Maximum time allowed to establish a connection.
  std::chrono::milliseconds connect{10'000};
  /// Maximum time allowed for one HTTP transfer.
  std::chrono::milliseconds transfer{120'000};
  /// Maximum time allowed for Scry-owned shutdown work.
  std::chrono::milliseconds shutdown{2'000};
};

/// Memory and admission limits applied by a Harness.
///
/// Every limit must be nonzero; zero is rejected by Harness::create().
struct ResourceLimits {
  /// Maximum accepted turns that may be active or queued in one Harness.
  std::size_t max_pending_turns{64};
  /// Maximum bytes in one decoded server-sent event.
  std::size_t max_sse_event_bytes{std::size_t{256} * 1024};
  /// Maximum cumulative response bytes accepted for one HTTP transfer.
  std::size_t max_response_bytes{std::size_t{8} * 1024 * 1024};
  /// Maximum serialized argument bytes across a tool call.
  std::size_t max_tool_arguments_bytes{std::size_t{1024} * 1024};
  /// Maximum serialized result bytes returned by one tool.
  std::size_t max_tool_result_bytes{std::size_t{4} * 1024 * 1024};
  /// Maximum queued callback payload bytes retained for one turn.
  std::size_t max_queued_event_bytes_per_turn{std::size_t{2} * 1024 * 1024};
  /// Maximum serialized bytes in a committed Conversation.
  std::size_t max_conversation_bytes{std::size_t{16} * 1024 * 1024};
};

/// Complete configuration used to create a Harness.
///
/// This is a designated-initializer-friendly value type. Changing dialects or pointing
/// at a local OpenAI-compatible server requires configuration changes only.
struct Config {
  /// Provider base URL or supported full endpoint.
  std::string base_url{};
  /// Provider credential. May be empty for an unauthenticated local server.
  std::string api_key{};
  /// Provider-specific model identifier.
  std::string model{};
  /// Wire protocol selected for this Harness.
  ProviderDialect dialect{ProviderDialect::anthropic};
  /// Sampling parameters.
  SamplingConfig sampling{};
  /// Retry behavior for transient pre-output failures.
  RetryPolicy retry{};
  /// Network and shutdown time bounds.
  TransportTimeouts timeouts{};
  /// Admission and memory limits.
  ResourceLimits limits{};
  /// Maximum tool-call rounds in one turn.
  std::uint32_t max_tool_rounds{8};
  /// Whether HTTPS peer certificates are verified.
  ///
  /// Disabling verification is intended only for explicitly trusted development
  /// endpoints.
  bool tls_verify_peer{true};
};

} // namespace scry
