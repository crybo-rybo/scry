#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace scry::test {

class LoopbackServer final {
public:
  explicit LoopbackServer(std::string response, bool hold_response = false);
  ~LoopbackServer();

  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  [[nodiscard]] std::string url(std::string_view path = "/") const;
  [[nodiscard]] std::string request() const;
  void wait_until_request();
  void release_response();

private:
  void serve(std::stop_token stop);

  int listener_{-1};
  unsigned short port_{};
  std::string response_{};
  mutable std::mutex state_mutex_{};
  std::condition_variable state_changed_{};
  std::string request_{};
  bool request_received_{false};
  bool response_released_{true};
  std::jthread thread_{};
};

} // namespace scry::test
