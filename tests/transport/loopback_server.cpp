#include "loopback_server.hpp"

#include <arpa/inet.h>
#include <array>
#include <charconv>
#include <cstddef>
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

// False if the client closed the connection before a complete request.
[[nodiscard]] bool receive_request(const int client) {
  std::string request;
  std::array<char, 4096> buffer{};
  while (!request_complete(request)) {
    const auto received = ::recv(client, buffer.data(), buffer.size(), 0);
    if (received <= 0) {
      return false;
    }
    request.append(buffer.data(), static_cast<std::size_t>(received));
  }
  return true;
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

LoopbackServer::LoopbackServer(std::string response,
                               const std::size_t requests_to_serve)
    : response_(std::move(response)), requests_to_serve_(requests_to_serve) {
  // create_listener throws rather than returning an invalid descriptor.
  listener_ = create_listener(port_);
  thread_ = std::jthread{[this](const std::stop_token stop) { serve(stop); }};
}

LoopbackServer::~LoopbackServer() {
  thread_.request_stop();
  ::shutdown(listener_, SHUT_RDWR);
  ::close(listener_);
  thread_.join();
}

std::string LoopbackServer::url() const {
  return "http://127.0.0.1:" + std::to_string(port_) + "/";
}

std::size_t LoopbackServer::accepted_connections() const {
  const std::scoped_lock lock{state_mutex_};
  return accepted_connections_;
}

void LoopbackServer::serve(const std::stop_token stop) {
  std::size_t served = 0;
  while (served < requests_to_serve_ && !stop.stop_requested()) {
    sockaddr_in address{};
    socklen_t size = sizeof(address);
    Socket client{::accept(listener_, reinterpret_cast<sockaddr*>(&address), &size)};
    if (client.get() < 0 || stop.stop_requested()) {
      return;
    }
    {
      const std::scoped_lock lock{state_mutex_};
      ++accepted_connections_;
    }
    // A connection serves requests until the client closes it.
    while (served < requests_to_serve_ && receive_request(client.get())) {
      send_all(client.get(), response_);
      ++served;
    }
  }
}

} // namespace scry::test
