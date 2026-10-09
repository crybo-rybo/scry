#pragma once

#include <cstddef>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace scry::test {

// Answers each request with fixed raw bytes. The transport tests use it only
// for responses that scry::testing::ScriptedServer cannot send: a declared
// Content-Length, a malformed header, an interim 1xx response, and a connection
// that stays open between two requests.
class LoopbackServer final {
public:
  // Serving more than one request keeps the accepted connection open between
  // them, so accepted_connections() distinguishes a reused connection from a
  // reconnect.
  explicit LoopbackServer(std::string response, std::size_t requests_to_serve = 1);
  ~LoopbackServer();

  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  [[nodiscard]] std::string url() const;
  [[nodiscard]] std::size_t accepted_connections() const;

private:
  void serve(std::stop_token stop);

  int listener_{-1};
  unsigned short port_{};
  std::string response_{};
  std::size_t requests_to_serve_{1};
  mutable std::mutex state_mutex_{};
  std::size_t accepted_connections_{};
  std::jthread thread_{};
};

} // namespace scry::test
