#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace scry::test {

class LoopbackServer final {
public:
  // Serving more than one request keeps the accepted connection open between
  // them, so accepted_connections() distinguishes a reused connection from a
  // reconnect.
  explicit LoopbackServer(std::string response, bool hold_response = false,
                          std::size_t requests_to_serve = 1);
  ~LoopbackServer();

  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  [[nodiscard]] std::string url(std::string_view path = "/") const;
  [[nodiscard]] std::string request() const;
  [[nodiscard]] std::size_t accepted_connections() const;
  void wait_until_request();
  void release_response();

private:
  void serve(std::stop_token stop);
  [[nodiscard]] bool serve_connection(int client, const std::stop_token& stop,
                                      std::size_t& served);

  int listener_{-1};
  unsigned short port_{};
  std::string response_{};
  std::size_t requests_to_serve_{1};
  mutable std::mutex state_mutex_{};
  std::condition_variable state_changed_{};
  std::string request_{};
  std::size_t accepted_connections_{};
  bool request_received_{false};
  bool response_released_{true};
  std::jthread thread_{};
};

} // namespace scry::test
