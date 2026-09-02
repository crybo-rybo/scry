#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// Stable C++23 API for the Scry runtime.
namespace scry {

/// Selects the wire protocol used by a Harness.
enum class ProviderDialect : std::uint8_t {
  /// Anthropic Messages API.
  anthropic,
  /// The supported OpenAI-compatible Chat Completions subset.
  openai_compatible,
};

/// Controls whether a provider may use its default reasoning behavior.
enum class ReasoningMode : std::uint8_t {
  /// Omit reasoning controls and use the provider or model default.
  provider_default,
  /// Request that reasoning be disabled when the selected dialect supports it.
  disabled,
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
  ///
  /// The Anthropic Messages API requires this field, so the Anthropic dialect
  /// rejects an unset value. It is optional for the OpenAI-compatible dialect:
  /// when unset the field is omitted from the request and the server default
  /// applies. Zero is rejected by both dialects.
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
  /// Maximum time allowed to establish a connection, including name resolution.
  std::chrono::milliseconds connect{10'000};
  /// Maximum time the response may stay silent. The transfer fails when no bytes
  /// arrive for this long, including while waiting for the first byte. Curl applies
  /// this in whole seconds; sub-second values round up to one second. Curl compares
  /// a rolling average rather than a strict gap, so a stall is reported some seconds
  /// after this bound rather than exactly at it.
  ///
  /// Local servers can spend minutes processing a long prompt before the first
  /// token; raise this bound for that deployment rather than disabling it.
  std::chrono::milliseconds idle{120'000};
  /// Optional maximum time for one whole HTTP transfer. Unset means the transfer is
  /// bounded only by `idle`, `connect`, and the configured byte limits, which is the
  /// right default for streaming responses of unknown length.
  std::optional<std::chrono::milliseconds> transfer{};
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

/// One HTTP request header appended verbatim to every provider request.
struct HttpHeader {
  /// Header name. Must be a non-empty RFC 7230 token.
  std::string name{};
  /// Header value. Must contain no carriage return or line feed.
  std::string value{};
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
  /// Reasoning behavior requested from the provider.
  ReasoningMode reasoning_mode{ReasoningMode::provider_default};
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
  /// Path to a PEM CA bundle used to verify the provider's certificate, for private
  /// or corporate CAs.
  ///
  /// Empty uses libcurl's default trust store. Supplying a bundle is the supported
  /// way to reach an internally signed endpoint; it does not require disabling
  /// tls_verify_peer.
  std::string ca_bundle_path{};
  /// Proxy URL passed to libcurl, for example "http://proxy.internal:3128".
  ///
  /// Empty leaves libcurl's default behavior, which honors the http_proxy family of
  /// environment variables.
  std::string proxy{};
  /// Headers appended verbatim to every request after the dialect's own headers.
  ///
  /// Names that collide with a Scry-managed header are rejected by
  /// Harness::create() and Harness::validate().
  std::vector<HttpHeader> extra_headers{};
};

} // namespace scry
