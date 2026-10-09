#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <poll.h>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <scry/testing/scripted_server.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace scry::testing {
namespace {

// How long a blocking step waits before it checks for shutdown or for a client
// that went away.
constexpr auto poll_period = std::chrono::milliseconds{10};

constexpr std::string_view last_chunk = "0\r\n\r\n";

// Linux reports a write to a closed peer through MSG_NOSIGNAL; macOS has no such
// flag and uses the SO_NOSIGPIPE socket option instead (see configure_client).
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif

class Socket final {
public:
  explicit Socket(const int descriptor = -1) noexcept : descriptor_{descriptor} {}
  ~Socket() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }

  Socket(Socket&& other) noexcept : descriptor_{std::exchange(other.descriptor_, -1)} {}
  Socket& operator=(Socket&&) = delete;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
  int descriptor_;
};

[[nodiscard]] Error listener_error(const std::string_view step) {
  return {
      .category = ErrorCategory::network,
      .message = "scripted server could not " + std::string{step},
  };
}

[[nodiscard]] Result<Socket> open_listener(std::uint16_t& port) {
  Socket listener{::socket(AF_INET, SOCK_STREAM, 0)};
  if (listener.get() < 0) {
    return std::unexpected(listener_error("create a socket"));
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  constexpr int backlog = 16;
  if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
      ::listen(listener.get(), backlog) != 0) {
    return std::unexpected(listener_error("listen on 127.0.0.1"));
  }
  socklen_t size = sizeof(address);
  if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &size) !=
      0) {
    return std::unexpected(listener_error("read its port"));
  }
  port = ntohs(address.sin_port);
  return listener;
}

void configure_client(const int client) {
  constexpr int enabled = 1;
  // Each body chunk goes out when it is written, not when Nagle allows it.
  static_cast<void>(
      ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)));
#ifdef SO_NOSIGPIPE
  static_cast<void>(
      ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)));
#endif
}

// Waits until the descriptor is ready for `events`. False means the server is
// stopping.
[[nodiscard]] bool wait_ready(const int descriptor, const short events,
                              const std::stop_token& stop) {
  pollfd entry{.fd = descriptor, .events = events, .revents = 0};
  while (!stop.stop_requested()) {
    const auto ready = ::poll(&entry, 1, static_cast<int>(poll_period.count()));
    if (ready > 0) {
      return true;
    }
    if (ready < 0 && errno != EINTR) {
      return false;
    }
  }
  return false;
}

// The client sends nothing after its request, so a readable socket means that
// it closed the connection or reset it.
[[nodiscard]] bool peer_closed(const int client) {
  pollfd entry{.fd = client, .events = POLLIN, .revents = 0};
  return ::poll(&entry, 1, 0) != 0;
}

// Never blocks in send, so a client that stops reading cannot keep the server
// from stopping. False if the client left or the server stops first.
[[nodiscard]] bool send_all(const int client, std::string_view bytes,
                            const std::stop_token& stop) {
  while (!bytes.empty()) {
    if (!wait_ready(client, POLLOUT, stop)) {
      return false;
    }
    const auto sent =
        ::send(client, bytes.data(), bytes.size(), send_flags | MSG_DONTWAIT);
    if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    if (sent <= 0) {
      return false;
    }
    bytes.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

[[nodiscard]] bool equal_ignoring_case(const std::string_view left,
                                       const std::string_view right) {
  return std::ranges::equal(left, right, [](const char lhs, const char rhs) {
    return std::tolower(static_cast<unsigned char>(lhs)) ==
           std::tolower(static_cast<unsigned char>(rhs));
  });
}

[[nodiscard]] std::string_view trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return {};
  }
  value.remove_prefix(first);
  return value.substr(0, value.find_last_not_of(" \t") + 1);
}

// Parses the request line and the header lines, without the blank line.
[[nodiscard]] CapturedRequest parse_head(std::string_view head) {
  CapturedRequest request;
  auto line_end = head.find("\r\n");
  const auto request_line = head.substr(0, line_end);
  const auto method_end = request_line.find(' ');
  request.method = request_line.substr(0, method_end);
  if (method_end != std::string_view::npos) {
    const auto target = request_line.substr(method_end + 1);
    request.target = target.substr(0, target.find(' '));
  }
  while (line_end != std::string_view::npos) {
    head.remove_prefix(line_end + 2);
    line_end = head.find("\r\n");
    const auto line = head.substr(0, line_end);
    const auto separator = line.find(':');
    if (separator != std::string_view::npos) {
      request.headers.push_back({
          .name = std::string{trim(line.substr(0, separator))},
          .value = std::string{trim(line.substr(separator + 1))},
      });
    }
  }
  return request;
}

[[nodiscard]] std::size_t content_length(const CapturedRequest& request) {
  const auto header = std::ranges::find_if(request.headers, [](const auto& entry) {
    return equal_ignoring_case(entry.name, "content-length");
  });
  std::size_t length = 0;
  if (header != request.headers.end()) {
    const auto& value = header->value;
    static_cast<void>(
        std::from_chars(value.data(), value.data() + value.size(), length));
  }
  return length;
}

// Reads one request: the head up to the blank line, then Content-Length body
// bytes. Empty if the client leaves or the server stops first.
[[nodiscard]] std::optional<CapturedRequest>
receive_request(const int client, const std::stop_token& stop) {
  std::string received;
  std::array<char, 4096> buffer{};
  std::optional<CapturedRequest> request;
  std::size_t body_start = 0;
  std::size_t body_size = 0;
  while (true) {
    if (!request) {
      const auto head_end = received.find("\r\n\r\n");
      if (head_end != std::string::npos) {
        request = parse_head(std::string_view{received}.substr(0, head_end));
        body_start = head_end + 4;
        body_size = content_length(*request);
      }
    }
    if (request && received.size() >= body_start + body_size) {
      request->body = received.substr(body_start, body_size);
      return request;
    }
    if (!wait_ready(client, POLLIN, stop)) {
      return std::nullopt;
    }
    const auto count = ::recv(client, buffer.data(), buffer.size(), 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return std::nullopt;
    }
    received.append(buffer.data(), static_cast<std::size_t>(count));
  }
}

[[nodiscard]] std::string response_head(const ScriptedResponse& response) {
  auto head = "HTTP/1.1 " + std::to_string(response.status) + " Scripted\r\n";
  const auto typed = std::ranges::any_of(response.headers, [](const auto& header) {
    return equal_ignoring_case(header.name, "content-type");
  });
  if (!typed) {
    const auto success = response.status >= 200 && response.status < 300;
    head += success ? "Content-Type: text/event-stream\r\n"
                    : "Content-Type: application/json\r\n";
  }
  for (const auto& header : response.headers) {
    head += header.name + ": " + header.value + "\r\n";
  }
  head += "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  return head;
}

// One HTTP chunk. An empty chunk would read as the last chunk, so it sends
// nothing.
[[nodiscard]] std::string chunk_frame(const std::string_view chunk) {
  if (chunk.empty()) {
    return {};
  }
  std::array<char, 2 * sizeof(std::size_t)> digits{};
  const auto end =
      std::to_chars(digits.data(), digits.data() + digits.size(), chunk.size(), 16).ptr;
  auto frame = std::string{digits.data(), end};
  frame += "\r\n";
  frame += chunk;
  frame += "\r\n";
  return frame;
}

// The body has the error shape of both dialects, so the provider_detail of the
// failure names the cause.
[[nodiscard]] ScriptedResponse unscripted_response() {
  return {
      .status = 404,
      .body_chunks = {R"({"type":"error","error":{"type":"unscripted_request",)"
                      R"("message":"the scripted server has no response left"}})"},
  };
}

} // namespace

// The server thread and the test threads share the script, the received
// requests, and the release count, so one lock covers them. The sockets belong
// to the server thread alone.
class ScriptedServer::Impl final {
public:
  Impl(Socket listener, const std::uint16_t port)
      : listener_{std::move(listener)}, port_{port},
        thread_{[this](const std::stop_token& stop) { serve(stop); }} {}

  [[nodiscard]] std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  void enqueue(ScriptedResponse response) {
    const std::scoped_lock lock{mutex_};
    responses_.push_back(std::move(response));
  }

  [[nodiscard]] std::vector<CapturedRequest> requests() const {
    const std::scoped_lock lock{mutex_};
    return requests_;
  }

  [[nodiscard]] std::size_t remaining() const {
    const std::scoped_lock lock{mutex_};
    return responses_.size();
  }

  [[nodiscard]] bool wait_for_request(const std::size_t count,
                                      const std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return requests_.size() >= count; });
  }

  void release() {
    {
      const std::scoped_lock lock{mutex_};
      ++releases_;
    }
    changed_.notify_all();
  }

private:
  void serve(const std::stop_token& stop) {
    while (wait_ready(listener_.get(), POLLIN, stop)) {
      const Socket client{::accept(listener_.get(), nullptr, nullptr)};
      if (client.get() >= 0) {
        configure_client(client.get());
        serve_connection(client.get(), stop);
      }
    }
  }

  // Each return before the last chunk closes the connection with the body
  // unfinished, which is what close_after_chunks asks for.
  void serve_connection(const int client, const std::stop_token& stop) {
    auto request = receive_request(client, stop);
    if (!request) {
      return;
    }
    const auto response = claim(std::move(*request));
    if (response.hold && !await_release(client, stop)) {
      return;
    }
    if (!send_all(client, response_head(response), stop)) {
      return;
    }
    const auto& chunks = response.body_chunks;
    for (std::size_t index = 0; index <= chunks.size(); ++index) {
      if (response.pause_after_chunks == index && !await_release(client, stop)) {
        return;
      }
      if (response.close_after_chunks == index) {
        return;
      }
      const auto frame =
          index < chunks.size() ? chunk_frame(chunks[index]) : std::string{last_chunk};
      if (!send_all(client, frame, stop)) {
        return;
      }
    }
  }

  [[nodiscard]] ScriptedResponse claim(CapturedRequest request) {
    auto response = unscripted_response();
    {
      const std::scoped_lock lock{mutex_};
      requests_.push_back(std::move(request));
      if (!responses_.empty()) {
        response = std::move(responses_.front());
        responses_.pop_front();
      }
    }
    changed_.notify_all();
    return response;
  }

  // Takes one release. False if the client left or the server stops first; the
  // release then stays for the next hold or pause.
  [[nodiscard]] bool await_release(const int client, const std::stop_token& stop) {
    std::unique_lock lock{mutex_};
    while (releases_ == 0) {
      if (stop.stop_requested() || peer_closed(client)) {
        return false;
      }
      static_cast<void>(
          changed_.wait_for(lock, poll_period, [this] { return releases_ > 0; }));
    }
    --releases_;
    return true;
  }

  mutable std::mutex mutex_{};
  std::condition_variable changed_{};
  std::deque<ScriptedResponse> responses_{};
  std::vector<CapturedRequest> requests_{};
  std::size_t releases_{};
  Socket listener_;
  std::uint16_t port_;
  // Last, so it starts after every other member exists and is stopped and
  // joined before any of them is destroyed.
  std::jthread thread_;
};

Result<ScriptedServer> ScriptedServer::create() {
  std::uint16_t port = 0;
  auto listener = open_listener(port);
  if (!listener) {
    return std::unexpected(std::move(listener.error()));
  }
  return ScriptedServer{std::make_unique<Impl>(std::move(*listener), port)};
}

ScriptedServer::ScriptedServer(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

ScriptedServer::~ScriptedServer() = default;

ScriptedServer::ScriptedServer(ScriptedServer&&) noexcept = default;

ScriptedServer& ScriptedServer::operator=(ScriptedServer&&) noexcept = default;

std::string ScriptedServer::url() const { return impl_->url(); }

void ScriptedServer::enqueue(ScriptedResponse response) {
  impl_->enqueue(std::move(response));
}

std::vector<CapturedRequest> ScriptedServer::requests() const {
  return impl_->requests();
}

std::size_t ScriptedServer::remaining() const { return impl_->remaining(); }

bool ScriptedServer::wait_for_request(const std::size_t count,
                                      const std::chrono::milliseconds timeout) {
  return impl_->wait_for_request(count, timeout);
}

void ScriptedServer::release() { impl_->release(); }

} // namespace scry::testing
