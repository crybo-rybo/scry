#pragma once

#include "core/transport.hpp"

#include <atomic>
#include <cstddef>
#include <deque>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace scry::test {

struct ScriptedExchange {
  std::vector<std::string> body_chunks{};
  Result<detail::TransportResult> result{detail::TransportResult{}};
};

class FakeTransport final : public detail::Transport {
public:
  void enqueue(ScriptedExchange exchange) { exchanges_.push_back(std::move(exchange)); }

  [[nodiscard]] const std::vector<detail::TransportRequest>& requests() const noexcept {
    return requests_;
  }

  [[nodiscard]] std::size_t remaining() const noexcept { return exchanges_.size(); }

  [[nodiscard]] Result<detail::TransportResult>
  perform(const detail::TransportRequest& request, const std::stop_token shutdown,
          const std::atomic<bool>& cancelled,
          detail::BodyChunkSink& body_sink) override {
    if (shutdown.stop_requested() || cancelled.load(std::memory_order_acquire)) {
      return std::unexpected(Error{
          .category = ErrorCategory::cancelled,
          .message = "scripted transport cancelled",
      });
    }
    if (exchanges_.empty()) {
      return std::unexpected(Error{
          .category = ErrorCategory::invalid_state,
          .message = "scripted transport has no queued exchange",
      });
    }
    requests_.push_back(request);
    auto exchange = std::move(exchanges_.front());
    exchanges_.pop_front();
    for (const auto& chunk : exchange.body_chunks) {
      if (shutdown.stop_requested() || cancelled.load(std::memory_order_acquire)) {
        return std::unexpected(Error{
            .category = ErrorCategory::cancelled,
            .message = "scripted transport cancelled",
        });
      }
      auto status = body_sink(chunk);
      if (!status) {
        return std::unexpected(std::move(status.error()));
      }
    }
    return std::move(exchange.result);
  }

private:
  std::deque<ScriptedExchange> exchanges_{};
  std::vector<detail::TransportRequest> requests_{};
};

} // namespace scry::test
