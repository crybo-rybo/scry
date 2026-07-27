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
#include <scry/harness.hpp>
#include <string>
#include <thread>
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

[[nodiscard]] detail::Message user_message(std::string text) {
  return detail::Message{
      .role = detail::Role::user,
      .content = {detail::TextBlock{.text = std::move(text)}},
  };
}

[[nodiscard]] detail::ModelRequest
make_request(const Config& config, const detail::ConversationState& conversation,
             std::vector<detail::Message> messages,
             std::vector<detail::ToolSchema> schemas) {
  return detail::ModelRequest{
      .model = config.model,
      .system_prompt = conversation.config.system_prompt,
      .messages = std::move(messages),
      .tools = std::move(schemas),
      .sampling = config.sampling,
  };
}

struct SynchronousTurnState {
  std::optional<Completion> completion{};
  std::optional<Error> error{};
  bool cancelled{false};

  [[nodiscard]] bool terminal() const noexcept {
    return completion.has_value() || error.has_value() || cancelled;
  }

  [[nodiscard]] Result<Completion> result(const TurnId turn_id) {
    if (completion) {
      return std::move(*completion);
    }
    if (error) {
      return std::unexpected(std::move(*error));
    }
    return std::unexpected(Error{
        .category = ErrorCategory::cancelled,
        .message = "turn was cancelled",
        .turn_id = turn_id,
    });
  }
};

void install_synchronous_callbacks(Turn& turn, SynchronousTurnState& state) {
  const auto text = turn.on_text_delta([](std::string_view) {});
  const auto completion = turn.on_completion(
      [&state](const Completion& value) { state.completion = value; });
  const auto error =
      turn.on_error([&state](const Error& value) { state.error = value; });
  const auto cancelled =
      turn.on_cancelled([&state](const Cancelled&) { state.cancelled = true; });
  static_cast<void>(text);
  static_cast<void>(completion);
  static_cast<void>(error);
  static_cast<void>(cancelled);
  assert(text && completion && error && cancelled);
}

} // namespace

class Harness::Impl final {
public:
  Impl(Config config, std::unique_ptr<detail::ProviderAdapter> provider,
       std::unique_ptr<detail::Transport> transport,
       std::unique_ptr<ToolRegistry> tools)
      : config_(std::move(config)), commands_(std::make_shared<detail::CommandQueue>()),
        events_(std::make_shared<detail::EventQueue>()), pump_(events_),
        tools_(std::move(tools)),
        worker_([config = config_, provider = std::move(provider),
                 transport = std::move(transport), commands = commands_,
                 events = events_](const std::stop_token& stopped) mutable {
          detail::WorkerActor actor{std::move(config), std::move(provider),
                                    std::move(transport), std::move(commands),
                                    std::move(events)};
          actor.run(stopped);
        }) {
    tools_->impl_->bind_command_queue(commands_);
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
       detail::ToolSnapshot tools) {
    if (text.empty()) {
      return std::unexpected(immediate_error(ErrorCategory::invalid_state,
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
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto messages = conversation->messages;
    messages.push_back(user_message(text));
    auto schemas = detail::snapshot_schemas(tools);
    auto worker_tool_names = detail::snapshot_worker_tool_names(tools);
    auto route = std::make_shared<detail::TurnRoute>(
        turn_id, cancelled, commands_, conversation, std::move(text),
        detail::TurnRouteOptions{
            .tools = std::move(tools),
            .max_tool_result_bytes = config_.limits.max_tool_result_bytes,
            .max_exchange_bytes = max_exchange_bytes,
            .max_conversation_bytes = config_.limits.max_conversation_bytes,
        });
    auto request =
        make_request(config_, *conversation, std::move(messages), std::move(schemas));

    conversation->busy = true;
    pump_.add_route(route);
    commands_->push(detail::SendTurnCommand{
        .turn_id = turn_id,
        .request = std::move(request),
        .worker_tool_names = std::move(worker_tool_names),
        .cancelled = std::move(cancelled),
        .max_exchange_bytes = max_exchange_bytes,
    });
    return route;
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

std::unique_ptr<ToolRegistry> Harness::make_tool_registry() {
  return std::unique_ptr<ToolRegistry>{
      new ToolRegistry{std::make_unique<ToolRegistry::Impl>()}};
}

Result<Harness> Harness::create(Config config) {
  if (auto status = detail::validate_config(config); !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto provider = detail::make_provider_adapter(config.dialect);
  if (!provider) {
    return std::unexpected(std::move(provider.error()));
  }
  auto transport = std::make_unique<detail::CurlTransport>();
  if (auto status = transport->status(); !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto tools = make_tool_registry();
  return detail::translate_worker_start_failure<Harness>(
      [config = std::move(config), provider = std::move(*provider),
       transport = std::move(transport), tools = std::move(tools)]() mutable {
        return Harness{std::make_unique<Impl>(std::move(config), std::move(provider),
                                              std::move(transport), std::move(tools))};
      });
}

ToolRegistry& Harness::tools() noexcept {
  assert(impl_ != nullptr);
  return impl_->tools();
}

const ToolRegistry& Harness::tools() const noexcept {
  assert(impl_ != nullptr);
  return impl_->tools();
}

Result<Turn> Harness::send(Conversation& conversation, std::string user_message_text) {
  if (impl_ == nullptr || conversation.impl_ == nullptr) {
    return std::unexpected(immediate_error(
        ErrorCategory::invalid_state, "Harness and Conversation must both be active"));
  }
  auto tools = impl_->tools().impl_->snapshot();
  auto route = impl_->send(conversation.impl_->state, std::move(user_message_text),
                           std::move(tools));
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
  auto turn_result = send(conversation, std::move(user_message_text));
  if (!turn_result) {
    return std::unexpected(std::move(turn_result.error()));
  }
  auto turn = std::move(*turn_result);
  SynchronousTurnState state;
  install_synchronous_callbacks(turn, state);
  while (!state.terminal()) {
    static_cast<void>(update());
    if (!state.terminal()) {
      static_cast<void>(impl_->wait_for_event());
    }
  }
  return state.result(turn.id());
}

UpdateStats Harness::update(const UpdateOptions options) {
  return impl_ == nullptr ? UpdateStats{} : impl_->update(options);
}

namespace detail {

Result<Harness> HarnessTestAccess::create(Config config,
                                          std::unique_ptr<ProviderAdapter> provider,
                                          std::unique_ptr<Transport> transport) {
  if (auto status = validate_config(config); !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (!provider || !transport) {
    return std::unexpected(
        immediate_error(ErrorCategory::invalid_config,
                        "provider and transport components must not be empty"));
  }
  auto tools = Harness::make_tool_registry();
  return translate_worker_start_failure<Harness>(
      [config = std::move(config), provider = std::move(provider),
       transport = std::move(transport), tools = std::move(tools)]() mutable {
        return Harness{
            std::make_unique<Harness::Impl>(std::move(config), std::move(provider),
                                            std::move(transport), std::move(tools))};
      });
}

} // namespace detail

} // namespace scry
