#include "core/provider.hpp"
#include "core/transport.hpp"
#include "runtime/test_access.hpp"
#include "transport/transport_policy.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <scry/harness.hpp>
#include <scry/testing/scripted_transport.hpp>
#include <scry/tool_registry.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scry::testing {
namespace {

[[nodiscard]] Error cancelled_error() {
  return {
      .category = ErrorCategory::cancelled,
      .message = "scripted transport cancelled",
  };
}

[[nodiscard]] Error exhausted_error() {
  return {
      .category = ErrorCategory::invalid_state,
      .message = "scripted transport has no queued response",
  };
}

[[nodiscard]] bool stopping(const std::stop_token& shutdown,
                            const std::atomic<bool>& cancelled) {
  return shutdown.stop_requested() || cancelled.load(std::memory_order_acquire);
}

// A scripted non-2xx status is classified exactly as a real one: the status
// picks the category and whether the runtime retries, and the same bounded body
// prefix CurlTransport keeps is mined for the provider's error token instead of
// being decoded as a stream.
[[nodiscard]] Error http_status_error(const ScriptedResponse& response,
                                      const std::string_view provider_namespace) {
  auto body = std::string{};
  for (const auto& chunk : response.body_chunks) {
    detail::transport_policy::append_error_body(body, chunk);
  }
  return detail::transport_policy::http_error(
      static_cast<std::int32_t>(response.status), response.request_id, body,
      provider_namespace);
}

// The Harness owns the transport it is created with, but a script has to stay
// readable afterwards, so what the Harness gets is this forwarder over the
// shared state rather than the state itself.
class SharedTransport final : public detail::Transport {
public:
  explicit SharedTransport(std::shared_ptr<detail::Transport> shared)
      : shared_{std::move(shared)} {}

  [[nodiscard]] Result<detail::TransportResult>
  perform(const detail::TransportRequest& request, const std::stop_token shutdown,
          const std::atomic<bool>& cancelled,
          detail::BodyChunkSink& body_sink) override {
    return shared_->perform(request, shutdown, cancelled, body_sink);
  }

private:
  std::shared_ptr<detail::Transport> shared_;
};

} // namespace

// Every member is written on the worker thread and read from the host thread,
// so the lock covers the script, the recorded requests, and the release flag
// alike; accessors hand back copies rather than references into live state.
class ScriptedTransport::Impl final : public detail::Transport {
public:
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

  [[nodiscard]] std::size_t calls() const {
    const std::scoped_lock lock{mutex_};
    return calls_;
  }

  void wait_for_call(const std::size_t count) {
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [this, count] { return calls_ >= count; });
  }

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
    if (stopping(shutdown, cancelled)) {
      return std::unexpected(cancelled_error());
    }
    auto claimed = claim(request, shutdown, cancelled);
    if (!claimed) {
      return std::unexpected(std::move(claimed.error()));
    }
    auto& response = *claimed;
    if (stopping(shutdown, cancelled)) {
      return std::unexpected(cancelled_error());
    }
    if (response.failure) {
      return std::unexpected(std::move(*response.failure));
    }
    if (response.status < 200 || response.status >= 300) {
      return std::unexpected(http_status_error(response, request.provider_namespace));
    }
    return stream(response, cancelled, shutdown, body_sink);
  }

private:
  // Records the request and takes the next scripted answer, blocking inside the
  // lock while a held answer waits for release().
  [[nodiscard]] Result<ScriptedResponse> claim(const detail::TransportRequest& request,
                                               const std::stop_token& shutdown,
                                               const std::atomic<bool>& cancelled) {
    std::unique_lock lock{mutex_};
    if (responses_.empty()) {
      return std::unexpected(exhausted_error());
    }
    ++calls_;
    requests_.push_back({
        .url = request.url,
        .headers = request.headers,
        .body = request.body,
    });
    auto response = std::move(responses_.front());
    responses_.pop_front();
    changed_.notify_all();
    if (response.hold && !await_release(lock, shutdown, cancelled)) {
      return std::unexpected(cancelled_error());
    }
    return response;
  }

  // Cancellation is an atomic with no notifier behind it, so a held transfer
  // polls it rather than sleeping until release(). Without that, cancelling a
  // held turn would strand the worker until the host released the script.
  [[nodiscard]] bool await_release(std::unique_lock<std::mutex>& lock,
                                   const std::stop_token& shutdown,
                                   const std::atomic<bool>& cancelled) {
    constexpr auto poll_period = std::chrono::milliseconds{2};
    while (!released_) {
      if (stopping(shutdown, cancelled)) {
        return false;
      }
      static_cast<void>(
          changed_.wait_for(lock, shutdown, poll_period, [this] { return released_; }));
    }
    return true;
  }

  [[nodiscard]] static Result<detail::TransportResult>
  stream(const ScriptedResponse& response, const std::atomic<bool>& cancelled,
         const std::stop_token& shutdown, detail::BodyChunkSink& body_sink) {
    for (const auto& chunk : response.body_chunks) {
      if (stopping(shutdown, cancelled)) {
        return std::unexpected(cancelled_error());
      }
      auto status = body_sink(chunk);
      if (!status) {
        return std::unexpected(std::move(status.error()));
      }
    }
    return detail::TransportResult{
        .status_code = response.status,
        .provider_request_id = response.request_id,
    };
  }

  mutable std::mutex mutex_{};
  std::condition_variable_any changed_{};
  std::deque<ScriptedResponse> responses_{};
  std::vector<CapturedRequest> requests_{};
  std::size_t calls_{};
  bool released_{false};
};

ScriptedTransport::ScriptedTransport() : impl_{std::make_shared<Impl>()} {}

ScriptedTransport::~ScriptedTransport() = default;

ScriptedTransport::ScriptedTransport(ScriptedTransport&&) noexcept = default;

ScriptedTransport& ScriptedTransport::operator=(ScriptedTransport&&) noexcept = default;

void ScriptedTransport::enqueue(ScriptedResponse response) {
  impl_->enqueue(std::move(response));
}

std::vector<CapturedRequest> ScriptedTransport::requests() const {
  return impl_->requests();
}

std::size_t ScriptedTransport::remaining() const { return impl_->remaining(); }

std::size_t ScriptedTransport::calls() const { return impl_->calls(); }

void ScriptedTransport::wait_for_call(const std::size_t count) {
  impl_->wait_for_call(count);
}

void ScriptedTransport::release() { impl_->release(); }

Result<Harness> create_harness(Config config, ScriptedTransport& transport,
                               ToolRegistry tools,
                               const std::uint64_t retry_jitter_seed) {
  auto provider = detail::make_provider_adapter(config.dialect);
  return detail::HarnessTestAccess::create(
      std::move(config), std::move(provider),
      std::make_unique<SharedTransport>(transport.impl_), retry_jitter_seed, {},
      std::move(tools));
}

} // namespace scry::testing
