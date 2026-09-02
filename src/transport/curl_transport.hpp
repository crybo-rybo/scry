/// @file
/// @brief RAII libcurl implementation of the internal transport seam.
///
/// Curl types and callback machinery are hidden behind a private implementation
/// so the neutral transport contract and installed public API remain free of
/// third-party declarations.

#pragma once

#include "core/transport.hpp"

#include <memory>

namespace scry::detail {

/// @brief Noncopyable production HTTP transport backed by libcurl multi.
///
/// Each instance owns a reusable multi handle on the Harness worker thread and creates
/// one easy handle per transfer. Process-global initialization is delegated to
/// `curl_global_status()`. C callback trampolines catch every exception and store
/// errors for value-based publication after curl returns.
class CurlTransport final : public Transport {
public:
  /// @brief Creates the private curl state and records initialization status.
  CurlTransport();
  /// @brief Releases all instance-owned curl resources through the PImpl.
  ~CurlTransport() override;

  /// Curl handles have one worker-thread owner and cannot be copied.
  CurlTransport(const CurlTransport&) = delete;
  /// Curl handles have one worker-thread owner and cannot be copy-assigned.
  CurlTransport& operator=(const CurlTransport&) = delete;
  /// Curl transport ownership is intentionally non-movable within its worker.
  CurlTransport(CurlTransport&&) = delete;
  /// Curl transport ownership is intentionally non-move-assignable within its worker.
  CurlTransport& operator=(CurlTransport&&) = delete;

  /// @brief Returns construction/global-runtime status before first use.
  /// @return Success or the cached initialization/capability error.
  [[nodiscard]] Status status() const;

  /// @brief Performs one bounded streaming transfer on the worker thread.
  /// @param request Validated provider-encoded request.
  /// @param shutdown Harness-wide stop token checked by progress polling.
  /// @param cancelled Per-turn atomic flag checked independently.
  /// @param body_sink Synchronous consumer for successful response chunks.
  /// @return Final metadata, or a categorized/correlated error.
  [[nodiscard]] Result<TransportResult> perform(const TransportRequest& request,
                                                std::stop_token shutdown,
                                                const std::atomic<bool>& cancelled,
                                                BodyChunkSink& body_sink) override;

private:
  /// @brief Curl-bearing implementation hidden from this header's consumers.
  class Impl;
  std::unique_ptr<Impl> impl_; ///< Exclusive worker-owned curl state.
};

} // namespace scry::detail
