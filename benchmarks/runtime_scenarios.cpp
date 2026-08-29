#include "machine/turn_machine.hpp"
#include "prepared_operation_impl.hpp"
#include "runtime/pump.hpp"
#include "scenario_support.hpp"
#include "scenarios.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>

namespace scry::bench {
namespace {

[[nodiscard]] std::string noncanonical_arguments(const std::size_t target_size,
                                                 const std::size_t index) {
  constexpr auto prefix = std::string_view{R"({ "z" : 2, "payload" : ")"};
  constexpr auto suffix = std::string_view{R"(", "a" : 1 })"};
  const auto minimum = prefix.size() + suffix.size();
  const auto padding = target_size > minimum ? target_size - minimum : 0;
  return std::string{prefix} + padded_text(padding, index) + std::string{suffix};
}

[[nodiscard]] detail::ModelRequest machine_request() {
  return {
      .system_prompt = "benchmark",
      .messages = {detail::Message{
          .role = detail::Role::user,
          .content = {detail::TextBlock{.text = "question"}},
      }},
  };
}

[[nodiscard]] RetryPolicy no_retry_policy() {
  return {
      .max_attempts = 1,
      .initial_backoff = std::chrono::milliseconds{0},
      .max_backoff = std::chrono::milliseconds{0},
      .max_elapsed = std::chrono::milliseconds{0},
      .jitter_ratio = 0.0,
  };
}

[[nodiscard]] bool began_turn(const detail::TransitionResult& transition) {
  return transition.status == detail::TransitionStatus::applied &&
         transition.commands.size() == 1 &&
         std::holds_alternative<detail::IssueModelRequest>(transition.commands.front());
}

void digest_published_calls(const detail::TransitionResult& transition,
                            std::uint64_t& digest, std::size_t& output_bytes,
                            bool& valid) {
  for (const auto& command : transition.commands) {
    const auto* published = std::get_if<detail::PublishToolCall>(&command);
    if (published == nullptr) {
      valid = false;
      continue;
    }
    digest_text(digest, published->call.id);
    digest_text(digest, published->call.name);
    digest_text(digest, published->call.arguments.text);
    output_bytes += detail::content_payload_bytes(published->call);
  }
}

[[nodiscard]] bool published_expected_calls(const detail::TransitionResult& transition,
                                            const std::size_t expected_count) noexcept {
  return transition.status == detail::TransitionStatus::applied &&
         transition.commands.size() == expected_count;
}

[[nodiscard]] bool
ready_for_next_model(const detail::TurnMachine& machine,
                     const detail::TransitionResult& transition) noexcept {
  return machine.phase() == detail::MachinePhase::awaiting_model &&
         began_turn(transition);
}

void observe_issued_request(const detail::TransitionResult& transition,
                            std::uint64_t& digest, bool& valid) {
  if (!valid) {
    return;
  }
  const auto& issued = std::get<detail::IssueModelRequest>(transition.commands.front());
  valid = issued.request != nullptr && issued.request->messages.size() == 3;
  if (issued.request != nullptr) {
    digest_number(digest, static_cast<std::uint64_t>(issued.request->messages.size()));
  }
}

[[nodiscard]] std::shared_ptr<detail::TurnRoute>
make_route(const std::uint64_t id,
           const std::shared_ptr<detail::CommandQueue>& commands,
           const std::shared_ptr<detail::ConversationState>& conversation,
           TurnCallbacks callbacks) {
  return std::make_shared<detail::TurnRoute>(
      TurnId{.value = id}, std::make_shared<std::atomic<bool>>(false), commands,
      conversation, padded_text(64, static_cast<std::size_t>(id)),
      detail::TurnRouteOptions{
          .max_tool_result_bytes = 1024,
          .max_conversation_bytes = std::size_t{4} * 1024 * 1024,
          .callbacks = std::move(callbacks),
      });
}

[[nodiscard]] detail::CompletionEvent completion_event(const TurnId turn_id,
                                                       const std::size_t seed) {
  return {
      .turn_id = turn_id,
      .exchange = {detail::Message{
          .role = detail::Role::assistant,
          .content = {detail::TextBlock{.text = padded_text(256, seed)}},
      }},
      .finish_reason = FinishReason::completed,
      .attempt_count = 1,
      .provider_request_id = "benchmark-request",
  };
}

[[nodiscard]] TurnCallbacks pump_callbacks(const PumpShape shape, std::uint64_t& digest,
                                           std::size_t& callback_count,
                                           bool& callbacks_valid, const bool validate) {
  TurnCallbacks callbacks{};
  if (shape == PumpShape::text_delivery) {
    callbacks.on_text_delta = [&digest, &callback_count,
                               validate](const std::string_view text) {
      if (validate) {
        digest_text(digest, text);
      }
      ++callback_count;
    };
    return callbacks;
  }
  callbacks.on_finished = [&digest, &callback_count, &callbacks_valid,
                           validate](Result<Completion> result) {
    if (!result) {
      callbacks_valid = false;
      return;
    }
    if (validate) {
      digest_text(digest, result->text);
      digest_text(digest, result->provider_request_id);
    }
    ++callback_count;
  };
  return callbacks;
}

[[nodiscard]] bool enqueue_pump_event(const PumpShape shape, const std::size_t index,
                                      const TurnId turn_id, detail::EventQueue& events,
                                      std::size_t& input_bytes) {
  constexpr auto byte_limit = std::size_t{1024} * 1024;
  if (shape == PumpShape::text_delivery) {
    auto text = padded_text(32, index);
    input_bytes += text.size();
    return events.push(
        detail::TextDeltaEvent{.turn_id = turn_id, .text = std::move(text)},
        byte_limit);
  }
  auto event = completion_event(turn_id, index);
  input_bytes += detail::event_payload_bytes(event);
  return events.push(std::move(event), byte_limit);
}

[[nodiscard]] std::size_t pump_output_bytes(
    const PumpShape shape,
    const std::vector<std::shared_ptr<detail::ConversationState>>& conversations,
    bool& valid) {
  if (shape == PumpShape::text_delivery) {
    return 32 * conversations.size();
  }
  std::size_t output_bytes = 0;
  for (const auto& conversation : conversations) {
    valid = valid && !conversation->busy && conversation->messages->size() == 2;
    output_bytes += conversation->payload_bytes;
  }
  return output_bytes;
}

} // namespace

TurnMachineScenario::TurnMachineScenario(const std::size_t tool_count,
                                         const std::size_t argument_bytes) {
  calls_.reserve(tool_count);
  results_.reserve(tool_count);
  for (std::size_t index = 0; index < tool_count; ++index) {
    auto arguments = noncanonical_arguments(argument_bytes, index);
    input_bytes_ += arguments.size();
    const auto id = "call-" + std::to_string(index);
    calls_.emplace_back(detail::ToolCallBlock{
        .id = id,
        .name = "lookup",
        .arguments = Json{.text = std::move(arguments)},
    });
    results_.push_back(detail::ToolResultBlock{
        .tool_call_id = id,
        .result = Json{.text = R"({"ok":true})"},
    });
  }
}

class TurnMachineOperationState final {
public:
  TurnMachineOperationState(std::vector<detail::ContentBlock> calls,
                            std::vector<detail::ToolResultBlock> tool_results,
                            const std::size_t logical_input_bytes,
                            const bool semantic_validation,
                            const ScenarioResult expected)
      : machine{TurnId{.value = 1}, machine_request(), no_retry_policy(),
                detail::ToolLoopPolicy{
                    .max_rounds = 2,
                    .max_argument_bytes = std::numeric_limits<std::size_t>::max(),
                }},
        response{.content = std::move(calls),
                 .finish_reason = FinishReason::tool_use,
                 .provider_request_id = "benchmark-request"},
        results{std::move(tool_results)}, oracle{expected},
        input_bytes{logical_input_bytes}, validate{semantic_validation} {
    setup_valid = began_turn(machine.apply(detail::BeginTurn{}));
  }

  [[nodiscard]] ScenarioResult run() {
    bool valid = setup_valid;
    auto published =
        machine.apply(detail::ModelCompleted{.response = std::move(response)});
    valid = published_expected_calls(published, results.size()) && valid;
    auto digest = validate ? fnv_offset : oracle.digest;
    auto output_bytes = std::size_t{0};
    if (validate) {
      digest_published_calls(published, digest, output_bytes, valid);
    }
    detail::TransitionResult transition{};
    for (auto& result : results) {
      if (validate) {
        digest_text(digest, result.tool_call_id);
        digest_text(digest, result.result.text);
        output_bytes += detail::content_payload_bytes(result);
      }
      transition = machine.apply(detail::ToolResultReady{
          .result = std::move(result),
          .observed_at = detail::MachineTimePoint{},
      });
    }
    valid = ready_for_next_model(machine, transition) && valid;
    if (validate) {
      observe_issued_request(transition, digest, valid);
    }
    return {
        .digest = digest,
        .input_bytes = static_cast<std::uint64_t>(input_bytes),
        .output_bytes =
            validate ? static_cast<std::uint64_t>(output_bytes) : oracle.output_bytes,
        .items = static_cast<std::uint64_t>(results.size()),
        .valid = valid && (validate || oracle.valid),
    };
  }

private:
  detail::TurnMachine machine;
  detail::ModelResponse response{};
  std::vector<detail::ToolResultBlock> results{};
  ScenarioResult oracle{};
  std::size_t input_bytes{};
  bool validate{};
  bool setup_valid{};
};

TurnMachineOperation TurnMachineScenario::make_operation(const bool validate) const {
  return TurnMachineOperation{std::make_unique<TurnMachineOperationState>(
      calls_, results_, input_bytes_, validate, oracle_)};
}

ScenarioResult TurnMachineScenario::validate() {
  auto operation = make_operation(true);
  oracle_ = operation.run();
  return oracle_;
}

TurnMachineOperation TurnMachineScenario::prepare() const {
  return make_operation(false);
}

PumpScenario::PumpScenario(const PumpShape shape, const std::size_t route_count)
    : shape_(shape), route_count_(route_count) {}

class PumpOperationState final {
public:
  PumpOperationState(const PumpShape pump_shape, const std::size_t route_count,
                     const bool semantic_validation, const ScenarioResult expected)
      : shape{pump_shape}, validate{semantic_validation}, oracle{expected},
        commands{std::make_shared<detail::CommandQueue>()},
        events{std::make_shared<detail::EventQueue>()},
        pump{events, [] { return std::chrono::steady_clock::time_point{}; }} {
    conversations.reserve(route_count);
    for (std::size_t index = 0; index < route_count; ++index) {
      auto conversation = std::make_shared<detail::ConversationState>();
      conversation->busy = true;
      auto callbacks =
          pump_callbacks(shape, digest, callback_count, setup_valid, validate);
      const auto route = make_route(static_cast<std::uint64_t>(index + 1), commands,
                                    conversation, std::move(callbacks));
      pump.add_route(route);
      conversations.push_back(std::move(conversation));
      setup_valid = setup_valid &&
                    enqueue_pump_event(shape, index, route->id(), *events, input_bytes);
    }
  }

  ~PumpOperationState() { pump.shutdown(); }

  [[nodiscard]] ScenarioResult run() {
    const auto stats = pump.update({});
    auto valid = setup_valid && callback_count == conversations.size() &&
                 stats.callbacks_delivered == conversations.size() &&
                 stats.events_remaining == 0 && !stats.budget_exhausted;
    const auto output_bytes = validate ? pump_output_bytes(shape, conversations, valid)
                                       : static_cast<std::size_t>(oracle.output_bytes);
    return {
        .digest = validate ? digest : oracle.digest,
        .input_bytes = static_cast<std::uint64_t>(input_bytes),
        .output_bytes = static_cast<std::uint64_t>(output_bytes),
        .items = static_cast<std::uint64_t>(callback_count),
        .valid = valid && (validate || oracle.valid),
    };
  }

private:
  PumpShape shape{};
  bool validate{};
  ScenarioResult oracle{};
  std::shared_ptr<detail::CommandQueue> commands{};
  std::shared_ptr<detail::EventQueue> events{};
  detail::PumpState pump;
  std::vector<std::shared_ptr<detail::ConversationState>> conversations{};
  std::uint64_t digest{fnv_offset};
  std::size_t callback_count{};
  std::size_t input_bytes{};
  bool setup_valid{true};
};

PumpOperation PumpScenario::make_operation(const bool validate) const {
  return PumpOperation{
      std::make_unique<PumpOperationState>(shape_, route_count_, validate, oracle_)};
}

ScenarioResult PumpScenario::validate() {
  auto operation = make_operation(true);
  oracle_ = operation.run();
  return oracle_;
}

PumpOperation PumpScenario::prepare() const { return make_operation(false); }

template class PreparedOperation<TurnMachineOperationState>;
template class PreparedOperation<PumpOperationState>;

} // namespace scry::bench
