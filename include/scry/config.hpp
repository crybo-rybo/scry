#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// Public C++26 API for the Scry runtime.
namespace scry {

/// Selects the wire protocol used by a Harness.
enum class ProviderDialect : std::uint8_t {
  /// Anthropic Messages API.
  anthropic,
  /// The supported OpenAI-compatible Chat Completions subset.
  openai_compatible,
  /// A local llamad daemon over gRPC on a Unix domain socket.
  ///
  /// Config::base_url is the gRPC target: `unix:` then a relative or absolute
  /// socket path, for example `unix:/run/user/1000/llamad.sock`, or `unix://` then
  /// an absolute one. The path must fit in `sun_path` with its terminator: at
  /// most 107 bytes on Linux and 103 on macOS. Config::model must still be
  /// non-empty, but the daemon serves the one model it loaded and ignores it. The
  /// daemon takes no credential, headers, proxy, or CA bundle, so those fields
  /// must stay empty, and it is reached without TLS, so Config::tls_verify_peer
  /// must stay true.
  ///
  /// The backend is compiled only when Scry is built with `SCRY_WITH_LLAMAD=ON`;
  /// without it, Harness::validate() and Harness::create() reject this dialect with
  /// ErrorCategory::invalid_config.
  llamad,
};

/// Controls whether a provider may use its default reasoning behavior.
enum class ReasoningMode : std::uint8_t {
  /// Omit reasoning controls and use the provider or model default.
  provider_default,
  /// Request that reasoning be disabled. Only the OpenAI-compatible dialect
  /// supports this; Harness::create() rejects it for the Anthropic and llamad
  /// dialects.
  disabled,
};

/// What a turn does when a model response requests tools past Config::max_tool_rounds.
enum class ToolRoundLimitPolicy : std::uint8_t {
  /// Fail the turn with ErrorCategory::max_tool_rounds and roll the Conversation
  /// back, discarding the rounds that already ran.
  fail,
  /// Complete the turn: commit the executed rounds and the final response's text,
  /// drop that response's tool-call blocks, and report
  /// FinishReason::tool_round_limit with the dropped calls in
  /// Completion::unexecuted_tool_calls.
  complete,
};

/// Sampling parameters sent with each model request.
///
/// Values are validated by Harness::create() for the selected provider dialect.
struct SamplingConfig {
  /// Sampling temperature. The llamad dialect treats 0 as greedy decoding.
  double temperature{1.0};
  /// Optional nucleus-sampling probability.
  std::optional<double> top_p{};
  /// Optional maximum number of output tokens requested from the provider.
  ///
  /// The Anthropic Messages API requires this field, so the Anthropic dialect
  /// rejects an unset value. It is optional for the OpenAI-compatible and llamad
  /// dialects: when unset the field is omitted from the request and the server
  /// default applies. Zero is rejected by every dialect, and llamad also rejects
  /// a value above 2147483647, the largest its wire field holds.
  std::optional<std::uint32_t> max_tokens{1024};
  /// Optional sampling seed, for repeatable experiments against one server.
  ///
  /// The OpenAI-compatible and llamad dialects send it when set and omit it when
  /// unset. The Anthropic Messages API has no seed, so the Anthropic dialect
  /// rejects a set value rather than drop it silently. Repeatability is the
  /// server's to provide, not Scry's: the same seed, model, prompt, and sampling
  /// values often reproduce an output on one server, but servers treat the seed as
  /// best-effort, and nothing carries across models, servers, or their versions.
  std::optional<std::uint32_t> seed{};
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
  /// For llamad it bounds the wait for the channel to report an outcome; the
  /// channel bounds each connection attempt itself, so a socket nobody is listening
  /// on, or a listener that never completes the handshake, fails at once rather
  /// than after this bound.
  std::chrono::milliseconds connect{10'000};
  /// Maximum time the response may stay silent. The transfer fails when no bytes
  /// arrive for this long, including while waiting for the first byte. Curl applies
  /// this in whole seconds; sub-second values round up to one second. Curl compares
  /// a rolling average rather than a strict gap, so a stall is reported some seconds
  /// after this bound rather than exactly at it.
  ///
  /// Local servers can spend minutes processing a long prompt before the first
  /// token; raise this bound for that deployment rather than disabling it.
  ///
  /// The llamad dialect measures this exactly, as the time since the last stream
  /// message arrived (or since the call started), without rounding.
  std::chrono::milliseconds idle{120'000};
  /// Optional maximum time for one whole HTTP transfer or llamad call. Unset means
  /// the transfer is bounded only by `idle`, `connect`, and the configured byte
  /// limits, which is the right default for streaming responses of unknown length.
  std::optional<std::chrono::milliseconds> transfer{};
  /// Maximum duration of one curl poll wait, or one llamad completion-queue or
  /// connection wait, before checking shutdown and cancellation again. This is not
  /// a hard deadline for the Harness destructor's worker join.
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
  /// Maximum cumulative response bytes accepted for one HTTP transfer or llamad
  /// call. For llamad it also caps the size of any one received gRPC message.
  std::size_t max_response_bytes{std::size_t{8} * 1024 * 1024};
  /// Maximum serialized argument bytes across a tool call.
  std::size_t max_tool_arguments_bytes{std::size_t{1024} * 1024};
  /// Maximum serialized result bytes returned by one tool.
  std::size_t max_tool_result_bytes{std::size_t{4} * 1024 * 1024};
  /// Maximum queued callback payload bytes retained for one turn: the text
  /// deltas, tool-call batches, and error diagnostics awaiting delivery.
  /// Completion payloads are charged to max_conversation_bytes instead, so a
  /// completion that fits the Conversation limit is always deliverable.
  std::size_t max_queued_event_bytes_per_turn{std::size_t{2} * 1024 * 1024};
  /// Maximum Conversation payload bytes, including the system prompt, text, tool
  /// identifiers and names, and serialized arguments/results. JSON envelope syntax
  /// and allocator overhead are excluded. The limit covers committed history plus
  /// the pending exchange and is checked by the Harness, not by from_json().
  std::size_t max_conversation_bytes{std::size_t{16} * 1024 * 1024};
};

/// One HTTP request header appended verbatim to every provider request.
struct HttpHeader {
  /// Header name. Must be a non-empty RFC 7230 token.
  std::string name{};
  /// Header value. Must contain no control characters other than tab.
  std::string value{};
};

/// Complete configuration used to create a Harness.
///
/// This is a designated-initializer-friendly value type. Changing dialects or pointing
/// at a local OpenAI-compatible server requires configuration changes only.
struct Config {
  /// Provider base URL or supported full endpoint, or for ProviderDialect::llamad
  /// a `unix:` gRPC target.
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
  /// Maximum tool calls dispatched to handlers in one turn, across every round.
  ///
  /// max_tool_rounds cannot bound this on its own, because one response may request
  /// many calls. Calls past the limit are refused with a fixed model-visible message
  /// instead of running their handler, and the turn continues. Unset means unlimited;
  /// zero is rejected by Harness::create() and Harness::validate().
  std::optional<std::uint32_t> max_tool_calls_per_turn{};
  /// What happens when a response requests tools past max_tool_rounds.
  ///
  /// The default fails the turn, which rolls back every round that already ran even
  /// though their handlers already changed host state. ToolRoundLimitPolicy::complete
  /// keeps host state and history in agreement instead.
  ToolRoundLimitPolicy tool_round_limit{ToolRoundLimitPolicy::fail};
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
