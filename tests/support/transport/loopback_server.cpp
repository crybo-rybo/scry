#include "support/transport/loopback_server.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace scry::test {
namespace {

class Socket final {
public:
  explicit Socket(const int descriptor = -1) noexcept : descriptor_(descriptor) {}
  ~Socket() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }

  Socket(Socket&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}

  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      if (descriptor_ >= 0) {
        ::close(descriptor_);
      }
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] int release() noexcept { return std::exchange(descriptor_, -1); }

private:
  int descriptor_{};
};

[[nodiscard]] std::size_t content_length(const std::string_view request) {
  constexpr std::string_view field = "Content-Length:";
  const auto start = request.find(field);
  if (start == std::string_view::npos) {
    return 0;
  }
  const auto value_start = request.find_first_not_of(" \t", start + field.size());
  const auto value_end = request.find("\r\n", value_start);
  if (value_start == std::string_view::npos || value_end == std::string_view::npos) {
    return 0;
  }
  std::size_t value{};
  const auto parsed =
      std::from_chars(request.data() + value_start, request.data() + value_end, value);
  return parsed.ec == std::errc{} ? value : 0;
}

[[nodiscard]] bool request_complete(const std::string& request) {
  const auto headers_end = request.find("\r\n\r\n");
  if (headers_end == std::string::npos) {
    return false;
  }
  const auto body_start = headers_end + 4;
  return request.size() >= body_start + content_length(request);
}

[[nodiscard]] std::string receive_request(const int client) {
  std::string request;
  std::array<char, 4096> buffer{};
  while (!request_complete(request)) {
    const auto received = ::recv(client, buffer.data(), buffer.size(), 0);
    if (received <= 0) {
      break;
    }
    request.append(buffer.data(), static_cast<std::size_t>(received));
  }
  return request;
}

void send_all(const int client, const std::string_view response) {
  std::size_t offset{};
  while (offset < response.size()) {
    const auto sent =
        ::send(client, response.data() + offset, response.size() - offset, 0);
    if (sent <= 0) {
      return;
    }
    offset += static_cast<std::size_t>(sent);
  }
}

[[nodiscard]] int create_listener(unsigned short& port) {
  Socket socket{::socket(AF_INET, SOCK_STREAM, 0)};
  if (socket.get() < 0) {
    throw std::runtime_error{"failed to create loopback socket"};
  }
  const int reuse = 1;
  if (::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) !=
      0) {
    throw std::runtime_error{"failed to configure loopback socket"};
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
      ::listen(socket.get(), 1) != 0) {
    throw std::runtime_error{"failed to bind loopback socket"};
  }
  socklen_t size = sizeof(address);
  if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &size) != 0) {
    throw std::runtime_error{"failed to inspect loopback socket"};
  }
  port = ntohs(address.sin_port);
  return socket.release();
}

} // namespace

LoopbackServer::LoopbackServer(std::string response, const bool hold_response)
    : response_(std::move(response)), response_released_(!hold_response) {
  listener_ = create_listener(port_);
  if (listener_ < 0) {
    throw std::runtime_error{"failed to retain loopback socket"};
  }
  thread_ = std::jthread{[this](const std::stop_token stop) { serve(stop); }};
}

LoopbackServer::~LoopbackServer() {
  thread_.request_stop();
  release_response();
  if (listener_ >= 0) {
    ::shutdown(listener_, SHUT_RDWR);
    thread_.join();
    ::close(listener_);
    listener_ = -1;
  }
}

std::string LoopbackServer::url(const std::string_view path) const {
  return "http://127.0.0.1:" + std::to_string(port_) + std::string{path};
}

std::string LoopbackServer::request() const {
  const std::scoped_lock lock{state_mutex_};
  return request_;
}

void LoopbackServer::wait_until_request() {
  std::unique_lock lock{state_mutex_};
  state_changed_.wait(lock, [this] { return request_received_; });
}

void LoopbackServer::release_response() {
  {
    const std::scoped_lock lock{state_mutex_};
    response_released_ = true;
  }
  state_changed_.notify_all();
}

void LoopbackServer::serve(const std::stop_token stop) {
  sockaddr_in address{};
  socklen_t size = sizeof(address);
  Socket client{::accept(listener_, reinterpret_cast<sockaddr*>(&address), &size)};
  if (client.get() < 0 || stop.stop_requested()) {
    return;
  }
  auto request = receive_request(client.get());
  {
    const std::scoped_lock lock{state_mutex_};
    request_ = std::move(request);
    request_received_ = true;
  }
  state_changed_.notify_all();
  {
    std::unique_lock lock{state_mutex_};
    state_changed_.wait(
        lock, [this, &stop] { return response_released_ || stop.stop_requested(); });
  }
  if (stop.stop_requested()) {
    return;
  }
  send_all(client.get(), response_);
}

} // namespace scry::test
