#include "core/provider.hpp"
#include "core/retry.hpp"
#include "runtime/config.hpp"
#include "runtime/conversation_impl.hpp"
#include "runtime/pump.hpp"
#include "runtime/startup.hpp"
#include "runtime/test_access.hpp"
#include "runtime/tool_registry_impl.hpp"
#include "runtime/turn_impl.hpp"
#include "runtime/worker.hpp"
#include "transport/curl_transport.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <scry/harness.hpp>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace scry {
namespace {

[[nodiscard]] Error immediate_error(const ErrorCategory category, std::string message) {
  return Error{
      .category = category,
      .message = std::move(message),
  };
}

[[nodiscard]] std::uint64_t make_retry_jitter_seed(const void* identity) noexcept {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto process = static_cast<std::uint64_t>(::getpid());
  const auto address =
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(identity));
  auto seed = now ^ (process << 32U) ^ address;
  try {
    std::random_device entropy;
    seed ^= static_cast<std::uint64_t>(entropy()) << 32U;
    seed ^= static_cast<std::uint64_t>(entropy());
  } catch (...) {
    // Mark the fallback domain; process, time, and allocation identity remain.
    seed ^= std::uint64_t{0xD1B54A32D192ED03};
  }
  return detail::mix_seed(seed);
}

[[nodiscard]] detail::Message user_message(std::string text) {
  return detail::Message{
      .role = detail::Role::user,
      .content = {detail::TextBlock{.text = std::move(text)}},
  };
}

[[nodiscard]] detail::ModelRequest
make_request(const Config& config, const detail::ConversationState& conversation,
             std::vector<detail::Message> messages, detail::SchemaSnapshot schemas) {
  return detail::ModelRequest{
      .system_prompt = conversation.config.system_prompt,
      .history = conversation.messages,
      .messages = std::move(messages),
      .tools = std::move(schemas),
      .sampling = config.sampling,
  };
}

// Keeps the send_and_wait() wait unwind-safe. That wait pumps callbacks for
// every accepted turn, so another turn's callback can throw straight through it
// while the waited turn is still running. Turn's destructor only detaches, and
// the route keeps the on_finished closure that writes into the wait's own stack
// slot, so the guard disconnects the waited turn on any exit that is not the
// normal one. The turn keeps running and still commits its history.
class WaitGuard final {
public:
  explicit WaitGuard(Turn& turn) noexcept : turn_(turn) {}
  WaitGuard(const WaitGuard&) = delete;
  WaitGuard(WaitGuard&&) = delete;
  WaitGuard& operator=(const WaitGuard&) = delete;
  WaitGuard& operator=(WaitGuard&&) = delete;
  ~WaitGuard() {
    if (!completed) {
      static_cast<void>(turn_.disconnect());
    }
  }

  bool completed{false};

private:
  Turn& turn_;
};

} // namespace

class Harness::Impl final {
public:
  /// Starts a Harness from components whose entry-point-specific checks have
  /// already succeeded.
  [[nodiscard]] static Result<Harness>
  start(Config config, std::unique_ptr<detail::ProviderAdapter> provider,
        std::unique_ptr<detail::Transport> transport, ToolRegistry tools,
        detail::WorkerEnvironment environment);

  Impl(Config config, std::unique_ptr<detail::ProviderAdapter> provider,
       std::unique_ptr<detail::Transport> transport, ToolRegistry tools,
       detail::WorkerEnvironment environment)
      : config_(std::move(config)), commands_(std::make_shared<detail::CommandQueue>()),
        events_(std::make_shared<detail::EventQueue>()), pump_(events_),
        tools_(std::move(tools)),
        worker_([config = config_, provider = std::move(provider),
                 transport = std::move(transport), commands = commands_,
                 events = events_, environment = std::move(environment)](
                    const std::stop_token& stopped) mutable {
          detail::WorkerActor actor{std::move(config),    std::move(provider),
                                    std::move(transport), std::move(commands),
                                    std::move(events),    std::move(environment)};
          actor.run(stopped);
        }) {
    // An adopted registry that was already moved from would otherwise leave
    // tools() permanently unusable; give this Harness an empty active one.
    detail::ToolRegistryAccess::ensure_active(tools_);
  }

  ~Impl() {
    worker_.request_stop();
    if (worker_.joinable()) {
      worker_.join();
    }
    pump_.shutdown();
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  [[nodiscard]] Result<std::shared_ptr<detail::TurnRoute>>
  send(const std::shared_ptr<detail::ConversationState>& conversation, std::string text,
       TurnCallbacks callbacks) {
    if (text.empty()) {
      return std::unexpected(immediate_error(ErrorCategory::invalid_argument,
                                             "user message must not be empty"));
    }
    if (conversation->busy) {
      return std::unexpected(immediate_error(
          ErrorCategory::busy, "Conversation already has a queued or active turn"));
    }
    if (pump_.live_route_count() >= config_.limits.max_pending_turns) {
      return std::unexpected(immediate_error(
          ErrorCategory::resource_limit, "Harness has reached the pending-turn limit"));
    }
    if (conversation->payload_bytes > config_.limits.max_conversation_bytes ||
        text.size() >
            config_.limits.max_conversation_bytes - conversation->payload_bytes) {
      return std::unexpected(
          immediate_error(ErrorCategory::resource_limit,
                          "user message exceeds the Conversation byte limit"));
    }
    if (next_turn_id_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(immediate_error(ErrorCategory::invalid_state,
                                             "Harness exhausted its Turn identifiers"));
    }

    const auto turn_id = TurnId{.value = ++next_turn_id_};
    const auto max_exchange_bytes = config_.limits.max_conversation_bytes -
                                    conversation->payload_bytes - text.size();
    auto tools = detail::ToolRegistryAccess::snapshot(tools_);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto messages = std::vector<detail::Message>{};
    messages.push_back(user_message(text));
    auto route = std::make_shared<detail::TurnRoute>(
        turn_id, cancelled, commands_, conversation, std::move(text),
        detail::TurnRouteOptions{
            .tools = std::move(tools.entries),
            .max_tool_result_bytes = config_.limits.max_tool_result_bytes,
            .max_exchange_bytes = max_exchange_bytes,
            .max_conversation_bytes = config_.limits.max_conversation_bytes,
            .callbacks = std::move(callbacks),
        });
    auto request = make_request(config_, *conversation, std::move(messages),
                                std::move(tools.schemas));

    conversation->busy = true;
    pump_.add_route(route);
    commands_->push(detail::SendTurnCommand{
        .turn_id = turn_id,
        .request = std::move(request),
        .cancelled = std::move(cancelled),
        .max_exchange_bytes = max_exchange_bytes,
    });
    return route;
  }

  [[nodiscard]] bool cancel(const TurnId turn_id) noexcept {
    const auto route = pump_.find_route(turn_id);
    return route != nullptr && route->cancel();
  }
  [[nodiscard]] bool disconnect(const TurnId turn_id) noexcept {
    const auto route = pump_.find_route(turn_id);
    return route != nullptr && route->disconnect();
  }
  [[nodiscard]] ToolRegistry& tools() noexcept { return tools_; }
  [[nodiscard]] const ToolRegistry& tools() const noexcept { return tools_; }
  [[nodiscard]] UpdateStats update(const UpdateOptions options) {
    return pump_.update(options);
  }
  [[nodiscard]] bool updating() const noexcept { return pump_.updating(); }
  [[nodiscard]] bool wait_for_event() {
    return events_->wait_for_data(std::chrono::milliseconds{10});
  }

private:
  Config config_{};
  std::shared_ptr<detail::CommandQueue> commands_{};
  std::shared_ptr<detail::EventQueue> events_{};
  detail::PumpState pump_;
  ToolRegistry tools_{};
  std::jthread worker_{};
  std::uint64_t next_turn_id_{};
};

Result<Harness> Harness::Impl::start(Config config,
                                     std::unique_ptr<detail::ProviderAdapter> provider,
                                     std::unique_ptr<detail::Transport> transport,
                                     ToolRegistry tools,
                                     detail::WorkerEnvironment environment) {
  return detail::translate_worker_start_failure<Harness>(
      [config = std::move(config), provider = std::move(provider),
       transport = std::move(transport), tools = std::move(tools),
       environment = std::move(environment)]() mutable {
        return Harness{std::make_unique<Impl>(std::move(config), std::move(provider),
                                              std::move(transport), std::move(tools),
                                              std::move(environment))};
      });
}

Harness::Harness(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Harness::~Harness() = default;
Harness::Harness(Harness&&) noexcept = default;
Harness& Harness::operator=(Harness&&) noexcept = default;

Result<Harness> Harness::create(Config config, ToolRegistry tools) {
  if (auto status = detail::validate_config(config); !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto provider = detail::make_provider_adapter(config.dialect);
  auto transport = std::make_unique<detail::CurlTransport>();
  if (auto status = transport->status(); !status) {
    return std::unexpected(std::move(status.error()));
  }
  const auto retry_jitter_seed = make_retry_jitter_seed(transport.get());
  return Impl::start(std::move(config), std::move(provider), std::move(transport),
                     std::move(tools),
                     detail::WorkerEnvironment{.retry_jitter_seed = retry_jitter_seed});
}

Status Harness::validate(const Config& config) {
  return detail::validate_config(config);
}

bool Harness::cancel(const TurnId turn_id) noexcept {
  if (impl_ == nullptr) {
    return false;
  }
  return impl_->cancel(turn_id);
}

bool Harness::disconnect(const TurnId turn_id) noexcept {
  if (impl_ == nullptr) {
    return false;
  }
  return impl_->disconnect(turn_id);
}

ToolRegistry& Harness::tools() noexcept {
  assert(impl_ != nullptr);
  return impl_->tools();
}

const ToolRegistry& Harness::tools() const noexcept {
  assert(impl_ != nullptr);
  return impl_->tools();
}

Result<Turn> Harness::send(Conversation& conversation, std::string user_message_text,
                           TurnCallbacks callbacks) {
  if (impl_ == nullptr || conversation.impl_ == nullptr) {
    return std::unexpected(immediate_error(
        ErrorCategory::invalid_state, "Harness and Conversation must both be active"));
  }
  auto route = impl_->send(conversation.impl_->state, std::move(user_message_text),
                           std::move(callbacks));
  if (!route) {
    return std::unexpected(std::move(route.error()));
  }
  return Turn{std::make_unique<Turn::Impl>(*route)};
}

Result<Completion> Harness::send_and_wait(Conversation& conversation,
                                          std::string user_message_text) {
  if (impl_ != nullptr && impl_->updating()) {
    return std::unexpected(
        immediate_error(ErrorCategory::invalid_state,
                        "send_and_wait cannot run from inside an update callback"));
  }
  // on_finished is guaranteed exactly once per accepted turn, so it alone decides
  // when this loop stops.
  std::optional<Result<Completion>> outcome;
  auto turn_result = send(
      conversation, std::move(user_message_text),
      TurnCallbacks{
          .on_finished =
              [&outcome](Result<Completion> result) { outcome = std::move(result); },
      });
  if (!turn_result) {
    return std::unexpected(std::move(turn_result.error()));
  }
  // The Turn is declared before the guard so it outlives the disconnect the
  // guard performs when the wait is abandoned.
  auto turn = std::move(*turn_result);
  WaitGuard guard{turn};
  while (!outcome) {
    static_cast<void>(update());
    if (!outcome) {
      static_cast<void>(impl_->wait_for_event());
    }
  }
  guard.completed = true;
  return std::move(*outcome);
}

UpdateStats Harness::update(const UpdateOptions options) {
  return impl_ == nullptr ? UpdateStats{} : impl_->update(options);
}

namespace detail {

Result<Harness> HarnessTestAccess::create(Config config,
                                          std::unique_ptr<ProviderAdapter> provider,
                                          std::unique_ptr<Transport> transport,
                                          const std::uint64_t retry_jitter_seed,
                                          WorkerTimeSource time, ToolRegistry tools) {
  if (auto status = validate_config(config); !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (!provider || !transport) {
    return std::unexpected(
        immediate_error(ErrorCategory::invalid_config,
                        "provider and transport components must not be empty"));
  }
  return Harness::Impl::start(std::move(config), std::move(provider),
                              std::move(transport), std::move(tools),
                              WorkerEnvironment{.retry_jitter_seed = retry_jitter_seed,
                                                .time = std::move(time)});
}

} // namespace detail

} // namespace scry
