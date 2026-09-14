#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <scry/harness.hpp>
#include <scry/tool_registry.hpp>
#include <string>
#include <vector>

/// @file
/// A Harness whose network is a script instead of a socket.
///
/// The scripted transport replaces only the HTTP transfer. Everything above it
/// is the shipping runtime: the worker thread, the provider request encoder and
/// stream decoder, retry scheduling, tool dispatch, transactional history, and
/// the pump that delivers callbacks on the host thread. A test written against
/// it therefore fails when Scry's behavior changes, not when a mock's
/// expectations drift.
namespace scry::testing {

class ScriptedTransport;

/// Creates a Harness whose transport is the supplied script.
///
/// The Harness does not take the handle: `transport` stays owned by the caller
/// and keeps working for inspection for as long as it lives, which may be
/// shorter or longer than the Harness. Retry jitter is seeded rather than
/// randomized, so a scripted retry sequence is reproducible.
///
/// Retry waits are real time, bounded by `config.retry`. Script a failure that
/// is retried only with a small initial_backoff and max_backoff, or the test
/// pays the production backoff in wall-clock seconds.
/// @param config Provider, retry, timeout, and resource configuration. The
/// dialect selects which provider encoder and decoder the turn runs through.
/// @param transport Script the worker performs its transfers against.
/// @param tools Registry to adopt, exactly as Harness::create adopts it.
/// @param retry_jitter_seed Seed for the per-attempt backoff jitter.
/// @return A Harness, or the same configuration or worker-start error
/// Harness::create would report.
[[nodiscard]] Result<Harness> create_harness(Config config,
                                             ScriptedTransport& transport,
                                             ToolRegistry tools = {},
                                             std::uint64_t retry_jitter_seed = 0);

/// One model request as the runtime handed it to the transport.
struct CapturedRequest {
  /// Fully resolved request URL, including the dialect's path suffix.
  std::string url{};
  /// Request headers, including the dialect's own and any Config extras.
  std::vector<HttpHeader> headers{};
  /// Encoded request body.
  std::string body{};
};

/// One scripted answer to one model request.
struct ScriptedResponse {
  /// HTTP status reported for the transfer.
  int status{200};
  /// Provider request identifier reported for the transfer.
  std::string request_id{};
  /// Raw response bytes for the configured dialect, delivered in order, so a
  /// test can split server-sent-event frames at any byte.
  std::vector<std::string> body_chunks{};
  /// Record the request, then block until release(); the turn stays in flight
  /// so the host can observe or cancel it mid-transfer.
  bool hold{false};
  /// Fail the transfer instead of answering it. A retryable category
  /// (rate_limit or network) drives the runtime's retry path.
  std::optional<Error> failure{};
};

/// A queue of scripted answers, plus the requests that consumed them.
///
/// The script is consumed in order, one entry per transfer. Requests are
/// recorded on the worker thread and read from the host thread, so every
/// accessor returns a copy taken under the handle's own lock. Performing a
/// transfer with an empty script fails that turn rather than blocking, so a
/// missing entry reads as a test error instead of a hang.
///
/// Use one handle from one Harness at a time.
class ScriptedTransport final {
public:
  /// Creates an empty script.
  ScriptedTransport();

  /// Releases the script. Outliving the Harness is fine, and so is the reverse.
  ~ScriptedTransport();

  /// Moves ownership of the script and its recorded requests.
  ScriptedTransport(ScriptedTransport&&) noexcept;

  /// Replaces this handle with another moved handle.
  /// @return This handle.
  ScriptedTransport& operator=(ScriptedTransport&&) noexcept;

  /// Scripts are not copyable.
  ScriptedTransport(const ScriptedTransport&) = delete;

  /// Scripts are not copy-assignable.
  ScriptedTransport& operator=(const ScriptedTransport&) = delete;

  /// Appends one answer to the end of the script.
  ///
  /// Safe to call while a Harness is running, which is how a test scripts the
  /// next round only after observing the previous one.
  /// @param response Answer the next unanswered transfer consumes.
  void enqueue(ScriptedResponse response);

  /// Returns every request the runtime has performed so far, in order.
  /// @return A copy of the recorded requests.
  [[nodiscard]] std::vector<CapturedRequest> requests() const;

  /// Returns how many scripted answers are still unconsumed.
  /// @return Remaining script length.
  [[nodiscard]] std::size_t remaining() const;

  /// Returns how many transfers have consumed a scripted answer.
  /// @return Number of performed transfers.
  [[nodiscard]] std::size_t calls() const;

  /// Blocks the calling thread until the runtime has started at least `count`
  /// transfers.
  ///
  /// A held answer records its transfer before it blocks, so this returns while
  /// the worker is still inside it.
  /// @param count Number of started transfers to wait for.
  void wait_for_call(std::size_t count);

  /// Releases every held answer, now and in the future.
  void release();

private:
  class Impl;

  std::shared_ptr<Impl> impl_;

  friend Result<Harness> create_harness(Config, ScriptedTransport&, ToolRegistry,
                                        std::uint64_t);
};

} // namespace scry::testing
