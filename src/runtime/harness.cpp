#include "core/log.hpp"
#include "core/provider.hpp"
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

[[nodiscard]] std::uint64_t mix_seed(std::uint64_t value) noexcept {
  value += std::uint64_t{0x9E3779B97F4A7C15};
  value = (value ^ (value >> 30U)) * std::uint64_t{0xBF58476D1CE4E5B9};
  value = (value ^ (value >> 27U)) * std::uint64_t{0x94D049BB133111EB};
  return value ^ (value >> 31U);
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
  return mix_seed(seed);
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

} // namespace

class Harness::Impl final {
public:
  /// ToolRegistry's constructor is private to its friend Harness, which extends to
  /// Harness's members. Keeping the factory here spares the public header a
  /// declaration that no consumer can use.
  [[nodiscard]] static std::unique_ptr<ToolRegistry> make_tool_registry() {
    return std::unique_ptr<ToolRegistry>{
        new ToolRegistry{std::make_unique<ToolRegistry::Impl>()}};
  }

  Impl(Config config, std::unique_ptr<detail::ProviderAdapter> provider,
       std::unique_ptr<detail::Transport> transport,
       std::unique_ptr<ToolRegistry> tools, const std::uint64_t retry_jitter_seed)
      : config_(std::move(config)), commands_(std::make_shared<detail::CommandQueue>()),
        events_(std::make_shared<detail::EventQueue>()), pump_(events_),
        tools_(std::move(tools)),
        worker_([config = config_, provider = std::move(provider),
                 transport = std::move(transport), commands = commands_,
                 events = events_,
                 retry_jitter_seed](const std::stop_token& stopped) mutable {
          detail::WorkerActor actor{std::move(config),    std::move(provider),
                                    std::move(transport), std::move(commands),
                                    std::move(events),    retry_jitter_seed};
          actor.run(stopped);
        }) {
    SCRY_LOG("Harness created (model: {})", config_.model);
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
    auto tools = tools_->impl_->snapshot();
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
    SCRY_LOG("Turn {} started", turn_id.value);
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
  [[nodiscard]] ToolRegistry& tools() noexcept { return *tools_; }
  [[nodiscard]] const ToolRegistry& tools() const noexcept { return *tools_; }
  [[nodiscard]] UpdateStats update(const UpdateOptions options) {
    return pump_.update(options);
  }
  [[nodiscard]] bool updating() const noexcept { return pump_.updating(); }
  [[nodiscard]] bool wait_for_event() {
    return events_->wait_for_data(std::chrono::milliseconds{10});
  }
  [[nodiscard]] bool has_current_tool_snapshot() const noexcept {
    const auto& state = tools_->impl_->state;
    return state.frozen.entries != nullptr &&
           state.frozen.entries->size() == state.entries.size();
  }

private:
  Config config_{};
  std::shared_ptr<detail::CommandQueue> commands_{};
  std::shared_ptr<detail::EventQueue> events_{};
  detail::PumpState pump_;
  std::unique_ptr<ToolRegistry> tools_{};
  std::jthread worker_{};
  std::uint64_t next_turn_id_{};
};

Harness::Harness(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Harness::~Harness() = default;
Harness::Harness(Harness&&) noexcept = default;
Harness& Harness::operator=(Harness&&) noexcept = default;

Result<Harness> Harness::create(Config config) {
  if (auto status = detail::validate_config(config); !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto provider = detail::make_provider_adapter(config.dialect);
  auto transport = std::make_unique<detail::CurlTransport>();
  if (auto status = transport->status(); !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto tools = Impl::make_tool_registry();
  const auto retry_jitter_seed = make_retry_jitter_seed(transport.get());
  return detail::translate_worker_start_failure<Harness>(
      [config = std::move(config), provider = std::move(provider),
       transport = std::move(transport), tools = std::move(tools),
       retry_jitter_seed]() mutable {
        return Harness{std::make_unique<Impl>(std::move(config), std::move(provider),
                                              std::move(transport), std::move(tools),
                                              retry_jitter_seed)};
      });
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
  const auto turn = std::move(*turn_result);
  while (!outcome) {
    static_cast<void>(update());
    if (!outcome) {
      static_cast<void>(impl_->wait_for_event());
    }
  }
  return std::move(*outcome);
}

UpdateStats Harness::update(const UpdateOptions options) {
  return impl_ == nullptr ? UpdateStats{} : impl_->update(options);
}

namespace detail {

Result<Harness> HarnessTestAccess::create(Config config,
                                          std::unique_ptr<ProviderAdapter> provider,
                                          std::unique_ptr<Transport> transport,
                                          const std::uint64_t retry_jitter_seed) {
  if (auto status = validate_config(config); !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (!provider || !transport) {
    return std::unexpected(
        immediate_error(ErrorCategory::invalid_config,
                        "provider and transport components must not be empty"));
  }
  auto tools = Harness::Impl::make_tool_registry();
  return translate_worker_start_failure<Harness>(
      [config = std::move(config), provider = std::move(provider),
       transport = std::move(transport), tools = std::move(tools),
       retry_jitter_seed]() mutable {
        return Harness{std::make_unique<Harness::Impl>(
            std::move(config), std::move(provider), std::move(transport),
            std::move(tools), retry_jitter_seed)};
      });
}

bool HarnessTestAccess::has_current_tool_snapshot(const Harness& harness) noexcept {
  return harness.impl_ != nullptr && harness.impl_->has_current_tool_snapshot();
}

} // namespace detail

} // namespace scry
