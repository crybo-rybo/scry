#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <string>
#include <vector>

/// @file
/// A scripted HTTP server on loopback for tests of a Scry host.
///
/// The server answers each request with the next scripted response. A test
/// sets `Config::base_url` to `url()` and calls the ordinary
/// `Harness::create`. Thus a scripted turn runs the shipping runtime and
/// transport: libcurl, HTTP/1.1, SSE framing, the provider request encoder and
/// stream decoder, retry scheduling, tool dispatch, transactional history, and
/// the pump that delivers callbacks on the host thread.
namespace scry::testing {

/// One HTTP request as the server received it.
struct CapturedRequest {
  /// Request method, for example `POST`.
  std::string method{};
  /// Request target, for example `/v1/messages`.
  std::string target{};
  /// Request headers in the order received, with names as sent.
  std::vector<HttpHeader> headers{};
  /// Request body.
  std::string body{};
};

/// One scripted answer to one HTTP request.
///
/// The server sends the status line, the headers, and then each body chunk as
/// one HTTP chunk of a chunked response, and it flushes each chunk separately.
/// The response always has `Connection: close`.
struct ScriptedResponse {
  /// HTTP status of the response.
  int status{200};
  /// Extra response headers, for example `Retry-After` or `request-id`. If no
  /// `Content-Type` is given, the server sends `text/event-stream` for a 2xx
  /// status and `application/json` for other statuses.
  std::vector<HttpHeader> headers{};
  /// Raw response body for the configured dialect, sent in order. A test can
  /// split server-sent-event frames at any byte.
  std::vector<std::string> body_chunks{};
  /// Record the request, then send no byte until a release(). The turn stays in
  /// flight, so the host can observe it or cancel it.
  bool hold{false};
  /// After this many body chunks, stop and wait for a release(). A host can
  /// then observe the delivered output, or cancel the turn.
  std::optional<std::size_t> pause_after_chunks{};
  /// After this many body chunks, close the connection without the end of the
  /// chunked body. libcurl reports a retryable network failure. With 0, no
  /// body byte arrives, so the runtime can retry the request.
  std::optional<std::size_t> close_after_chunks{};
};

/// An HTTP server on 127.0.0.1 that answers requests from a script.
///
/// The server listens on an ephemeral port and serves one connection at a time
/// on its own thread. It consumes the script in order, one response for each
/// request. A request that arrives when the script is empty gets status 404
/// with an error body of type `unscripted_request`. Scry reports this as a
/// `protocol` failure whose provider_detail ends in `:unscripted_request`.
///
/// If a client disconnects during a hold or a pause, for example because the
/// host cancelled the turn, the server stops that response and serves the next
/// connection. libcurl can take up to `Config::timeouts.shutdown` to see a
/// cancelled turn, so set that bound low in tests that cancel.
///
/// All member functions are thread-safe. Accessors return copies.
class ScriptedServer final {
public:
  /// Starts a server with an empty script.
  /// @return The server, or a `network` error if the loopback listener cannot
  /// be opened.
  [[nodiscard]] static Result<ScriptedServer> create();

  /// Stops the server and joins its thread. Open connections are closed.
  ~ScriptedServer();

  /// Moves the server. A moved-from server can only be destroyed or assigned.
  ScriptedServer(ScriptedServer&&) noexcept;

  /// Replaces this server with another moved server.
  /// @return This server.
  ScriptedServer& operator=(ScriptedServer&&) noexcept;

  /// Servers are not copyable.
  ScriptedServer(const ScriptedServer&) = delete;

  /// Servers are not copy-assignable.
  ScriptedServer& operator=(const ScriptedServer&) = delete;

  /// Returns the base URL of the server, for example `http://127.0.0.1:49152`.
  /// It is a valid `Config::base_url` for each dialect.
  /// @return The base URL.
  [[nodiscard]] std::string url() const;

  /// Appends one response to the end of the script.
  ///
  /// A test can call this while a Harness runs, for example to script the next
  /// round only after it observed the previous round.
  /// @param response Answer for the next request that has no answer.
  void enqueue(ScriptedResponse response);

  /// Returns each request that the server received, in order.
  /// @return A copy of the received requests.
  [[nodiscard]] std::vector<CapturedRequest> requests() const;

  /// Returns how many scripted responses are not consumed.
  /// @return The remaining script length.
  [[nodiscard]] std::size_t remaining() const;

  /// Blocks until the server has received at least `count` requests.
  ///
  /// The server records a request before it holds the response, so this
  /// returns while a held turn is still in flight.
  /// @param count Number of requests to wait for.
  /// @param timeout Longest time to wait.
  /// @return True if the server received `count` requests before the timeout.
  [[nodiscard]] bool
  wait_for_request(std::size_t count,
                   std::chrono::milliseconds timeout = std::chrono::seconds{10});

  /// Lets one hold or pause continue. If the server does not wait at a hold or
  /// a pause now, the next hold or pause that it gets to continues at once.
  void release();

private:
  class Impl;

  explicit ScriptedServer(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace scry::testing
