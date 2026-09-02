/// @file
/// @brief Provider-independent request/response values and transport seam.
///
/// The worker drives this interface synchronously on its own thread. Tests can
/// inject a deterministic fake; production uses the RAII curl implementation.
/// No curl declaration crosses this boundary.

#pragma once

#include <atomic>
#include <cstdint>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <scry/unique_function.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace scry::detail {

/// @brief Owning HTTP header name/value pair.
struct HttpHeader {
  std::string name{};  ///< Header field name without a trailing colon.
  std::string value{}; ///< Header field value without line terminators.
};

/// @brief Complete provider-encoded input to one blocking transfer.
struct TransportRequest {
  std::string url{}; ///< Absolute endpoint selected by the provider adapter.
  std::vector<HttpHeader> headers{}; ///< Outgoing headers validated by transport.
  std::string body{};                ///< Owning request body, normally canonical JSON.
  bool tls_verify_peer{true};        ///< Enables certificate and host verification.
  TransportTimeouts timeouts{};      ///< Positive connect/transfer/shutdown bounds.
  ResourceLimits limits{};           ///< Response and event byte ceilings.
};

/// @brief HTTP metadata returned after a successful transport operation.
///
/// Response body bytes are streamed separately through `BodyChunkSink`; they
/// are intentionally not retained here.
struct TransportResult {
  std::int32_t status_code{};        ///< Final HTTP response status.
  std::vector<HttpHeader> headers{}; ///< Final response header block.
  std::string provider_request_id{}; ///< Bounded correlation ID, if supplied.
};

/// @brief Move-only callback that consumes one bounded response-body chunk.
///
/// Returning an error aborts the transfer; the transport preserves that error
/// instead of replacing it with a generic curl callback failure.
using BodyChunkSink = UniqueFunction<Status(std::string_view)>;

/// @brief Injectable blocking HTTP transport used only by the worker.
///
/// Implementations must observe both cancellation signals, apply configured
/// resource limits, and convert all failures to `Error` values. No exception
/// may unwind through a C callback or across the worker boundary.
class Transport {
public:
  /// @brief Enables destruction through the transport seam.
  virtual ~Transport() = default;

  /// @brief Performs one streaming HTTP transfer on the calling worker thread.
  /// @param request Immutable encoded request; must outlive the call.
  /// @param shutdown Harness-wide stop token.
  /// @param cancelled Per-turn cooperative cancellation flag.
  /// @param body_sink Callback invoked synchronously for accepted 2xx body
  ///        chunks; its string view is valid only for the invocation.
  /// @return Final HTTP metadata, or a categorized transport/protocol/resource/
  ///         cancellation error.
  /// @note `shutdown` and `cancelled` have deliberately distinct scopes.
  [[nodiscard]] virtual Result<TransportResult>
  perform(const TransportRequest& request, std::stop_token shutdown,
          const std::atomic<bool>& cancelled, BodyChunkSink& body_sink) = 0;
};

} // namespace scry::detail
