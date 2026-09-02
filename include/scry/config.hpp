#pragma once

/**
 * @file config.hpp
 * @brief Provider, retry, timeout, and resource configuration for a Scry runtime.
 *
 * The types in this file are plain C++23 value aggregates. They are intentionally
 * suitable for designated initialization and contain no runtime resources. Pass a
 * completed Config to Harness::create(), which validates the values as one coherent
 * provider configuration before any turn can be accepted.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

/// Stable C++23 API for the Scry runtime.
namespace scry {

/// Selects the provider wire protocol used by a Harness.
///
/// A dialect controls endpoint normalization, authentication headers, request
/// encoding, and streaming response decoding. It is fixed for the lifetime of the
/// Harness created from the configuration.
enum class ProviderDialect : std::uint8_t {
  /// Anthropic Messages API.
  anthropic,
  /// The supported OpenAI-compatible Chat Completions subset.
  openai_compatible,
};

/// Controls provider-specific model reasoning behavior.
///
/// This is deliberately a small, portable control rather than a mirror of every
/// provider extension. Harness::create() rejects combinations unsupported by the
/// selected ProviderDialect.
enum class ReasoningMode : std::uint8_t {
  /// Omit reasoning controls and use the provider or model default.
  provider_default,
  /// Request that reasoning be disabled when the selected dialect supports it.
  disabled,
};

/// Sampling parameters sent with each model request.
///
/// Values are validated by Harness::create() for the selected provider dialect.
/// `top_p` may be omitted from a provider request; `max_tokens` uses an optional
/// representation but must currently contain a positive value.
struct SamplingConfig {
  /// Sampling temperature: `[0, 1]` for Anthropic and `[0, 2]` for OpenAI-compatible.
  double temperature{1.0};
  /// Optional nucleus-sampling probability.
  ///
  /// When present, the value must be in `(0, 1]` for Anthropic or `[0, 1]` for
  /// OpenAI-compatible. An empty value omits the control from the request.
  std::optional<double> top_p{};
  /// Positive maximum number of output tokens requested from the provider.
  ///
  /// Harness::create() rejects an empty or zero value for every supported dialect.
  std::optional<std::uint32_t> max_tokens{1024};
};

/// Retry limits and exponential-backoff settings for transient failures.
///
/// The policy applies independently to each model request in a multi-round turn.
/// Automatic retry is considered only before semantic model output has been consumed;
/// failures after partial output are returned to the application even when retryable.
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
  ///
  /// A value of `0` disables jitter; valid values are in the closed interval `[0, 1]`.
  double jitter_ratio{0.2};
};

/// Time bounds for Scry-owned network and shutdown operations.
///
/// All durations must be positive. These bounds apply to Scry-owned work only: a tool
/// handler or callback running inside Harness::update() is application code and is not
/// preempted by them.
struct TransportTimeouts {
  /// Maximum time allowed to establish a connection.
  std::chrono::milliseconds connect{10'000};
  /// Maximum time allowed for one HTTP transfer.
  std::chrono::milliseconds transfer{120'000};
  /// Maximum wait interval used while driving bounded Scry-owned shutdown work.
  std::chrono::milliseconds shutdown{2'000};
};

/// Memory and admission limits applied by a Harness.
///
/// Every limit must be nonzero; zero is rejected by Harness::create(). The queued-event
/// limit additionally has the field-specific minimum documented below. Payload limits
/// exclude allocator overhead; each field documents the bytes it counts. Crossing a
/// limit during an accepted turn terminates that turn with
/// ErrorCategory::resource_limit.
struct ResourceLimits {
  /// Maximum accepted turns that may be active or queued in one Harness.
  ///
  /// Admission above this bound fails synchronously; it does not evict an older turn.
  std::size_t max_pending_turns{64};
  /// Maximum bytes in one decoded server-sent event.
  std::size_t max_sse_event_bytes{std::size_t{256} * 1024};
  /// Maximum cumulative response bytes accepted for one HTTP transfer.
  std::size_t max_response_bytes{std::size_t{8} * 1024 * 1024};
  /// Maximum serialized argument bytes in one provider-requested tool call.
  std::size_t max_tool_arguments_bytes{std::size_t{1024} * 1024};
  /// Maximum serialized result bytes returned by one tool.
  std::size_t max_tool_result_bytes{std::size_t{4} * 1024 * 1024};
  /// Maximum queued callback payload bytes retained for one turn; at least 1024.
  ///
  /// This bounds memory when the host does not call Harness::update() promptly.
  std::size_t max_queued_event_bytes_per_turn{std::size_t{2} * 1024 * 1024};
  /// Maximum semantic Conversation payload, including the in-progress exchange.
  ///
  /// The system prompt, text, tool identifiers and names, JSON bytes, and tool-error
  /// markers share this cumulative budget. Canonical persistence framing and allocator
  /// overhead are not counted.
  std::size_t max_conversation_bytes{std::size_t{16} * 1024 * 1024};
};

/// Complete configuration used to create a Harness.
///
/// This is a designated-initializer-friendly value type. Changing dialects or pointing
/// at a local OpenAI-compatible server requires configuration changes only. A Harness
/// owns the configuration passed to Harness::create(); changing the original value
/// later does not reconfigure an existing runtime.
struct Config {
  /// Provider base URL or supported full endpoint.
  ///
  /// OpenAI-compatible configurations accept an origin, a `/v1` base, or a complete
  /// `/v1/chat/completions` endpoint and normalize it during Harness creation.
  std::string base_url{};
  /// Provider credential, stored privately by the Harness.
  ///
  /// This may be empty for an unauthenticated local OpenAI-compatible server. Scry
  /// redacts credentials and authorization headers from its diagnostics.
  std::string api_key{};
  /// Provider-specific model identifier.
  std::string model{};
  /// Wire protocol selected for this Harness.
  ProviderDialect dialect{ProviderDialect::anthropic};
  /// Sampling parameters.
  SamplingConfig sampling{};
  /// Reasoning behavior requested from the provider.
  ReasoningMode reasoning_mode{ReasoningMode::provider_default};
  /// Retry behavior for transient pre-output failures.
  RetryPolicy retry{};
  /// Network and shutdown time bounds.
  TransportTimeouts timeouts{};
  /// Admission and memory limits.
  ResourceLimits limits{};
  /// Maximum model-to-tool round trips accepted in one turn; must be positive.
  std::uint32_t max_tool_rounds{8};
  /// Whether HTTPS peer certificates are verified.
  ///
  /// Disabling verification is intended only for explicitly trusted development
  /// endpoints.
  bool tls_verify_peer{true};
};

} // namespace scry
