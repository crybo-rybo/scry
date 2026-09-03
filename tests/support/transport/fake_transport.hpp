#pragma once

#include "core/transport.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace scry::test {

struct ScriptedExchange {
  std::vector<std::string> body_chunks{};
  Result<detail::TransportResult> result{detail::TransportResult{}};
  // A held exchange records its request and then blocks inside perform() until
  // release() is called, so a test can observe the worker mid-transfer without
  // defining its own gated transport.
  bool hold{false};
};

// Scripted transport shared by every suite that drives a real Harness. All
// state is written on the worker thread and read from the test thread, so every
// member is guarded; requests() therefore returns a copy taken under the lock
// rather than a reference into live state.
class FakeTransport final : public detail::Transport {
public:
  void enqueue(ScriptedExchange exchange) {
    const std::scoped_lock lock{mutex_};
    exchanges_.push_back(std::move(exchange));
  }

  [[nodiscard]] std::vector<detail::TransportRequest> requests() const {
    const std::scoped_lock lock{mutex_};
    return requests_;
  }

  [[nodiscard]] std::size_t remaining() const {
    const std::scoped_lock lock{mutex_};
    return exchanges_.size();
  }

  // Number of perform() invocations that consumed a scripted exchange.
  [[nodiscard]] std::size_t calls() const {
    const std::scoped_lock lock{mutex_};
    return calls_;
  }

  // Blocks the test thread until the worker has entered perform() at least
  // `count` times. A held exchange records its call before it blocks, so this
  // returns while the worker is still inside the transfer.
  void wait_for_call(const std::size_t count) {
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [this, count] { return calls_ >= count; });
  }

  // Releases every held exchange, now and in the future.
  void release() {
    {
      const std::scoped_lock lock{mutex_};
      released_ = true;
    }
    changed_.notify_all();
  }

  [[nodiscard]] Result<detail::TransportResult>
  perform(const detail::TransportRequest& request, const std::stop_token shutdown,
          const std::atomic<bool>& cancelled,
          detail::BodyChunkSink& body_sink) override {
    if (shutdown.stop_requested() || cancelled.load(std::memory_order_acquire)) {
      return std::unexpected(cancelled_error());
    }
    ScriptedExchange exchange{};
    {
      std::unique_lock lock{mutex_};
      if (exchanges_.empty()) {
        return std::unexpected(Error{
            .category = ErrorCategory::invalid_state,
            .message = "scripted transport has no queued exchange",
        });
      }
      ++calls_;
      requests_.push_back(request);
      exchange = std::move(exchanges_.front());
      exchanges_.pop_front();
      changed_.notify_all();
      if (exchange.hold &&
          !changed_.wait(lock, shutdown, [this] { return released_; })) {
        return std::unexpected(cancelled_error());
      }
    }
    if (exchange.hold &&
        (shutdown.stop_requested() || cancelled.load(std::memory_order_acquire))) {
      return std::unexpected(cancelled_error());
    }
    for (const auto& chunk : exchange.body_chunks) {
      if (shutdown.stop_requested() || cancelled.load(std::memory_order_acquire)) {
        return std::unexpected(cancelled_error());
      }
      auto status = body_sink(chunk);
      if (!status) {
        return std::unexpected(std::move(status.error()));
      }
    }
    return std::move(exchange.result);
  }

private:
  [[nodiscard]] static Error cancelled_error() {
    return {
        .category = ErrorCategory::cancelled,
        .message = "scripted transport cancelled",
    };
  }

  mutable std::mutex mutex_{};
  std::condition_variable_any changed_{};
  std::deque<ScriptedExchange> exchanges_{};
  std::vector<detail::TransportRequest> requests_{};
  std::size_t calls_{};
  bool released_{false};
};

} // namespace scry::test
