#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace scry {

enum class ProviderDialect : std::uint8_t {
  anthropic,
  openai_compatible,
};

struct SamplingConfig {
  double temperature{1.0};
  std::optional<double> top_p{};
  std::optional<std::uint32_t> max_tokens{};
};

struct RetryPolicy {
  std::uint32_t max_attempts{3};
  std::chrono::milliseconds initial_backoff{250};
  std::chrono::milliseconds max_backoff{10'000};
  std::chrono::milliseconds max_elapsed{30'000};
  double jitter_ratio{0.2};
};

struct TransportTimeouts {
  std::chrono::milliseconds connect{10'000};
  std::chrono::milliseconds transfer{120'000};
  std::chrono::milliseconds shutdown{2'000};
};

struct ResourceLimits {
  std::size_t max_pending_turns{64};
  std::size_t max_sse_event_bytes{std::size_t{256} * 1024};
  std::size_t max_response_bytes{std::size_t{8} * 1024 * 1024};
  std::size_t max_tool_arguments_bytes{std::size_t{1024} * 1024};
  std::size_t max_tool_result_bytes{std::size_t{4} * 1024 * 1024};
  std::size_t max_queued_event_bytes_per_turn{std::size_t{2} * 1024 * 1024};
  std::size_t max_conversation_bytes{std::size_t{16} * 1024 * 1024};
};

struct Config {
  std::string base_url{};
  std::string api_key{};
  std::string model{};
  ProviderDialect dialect{ProviderDialect::anthropic};
  SamplingConfig sampling{};
  RetryPolicy retry{};
  TransportTimeouts timeouts{};
  ResourceLimits limits{};
  std::uint32_t max_tool_rounds{8};
  bool tls_verify_peer{true};
};

} // namespace scry
