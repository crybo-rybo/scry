#include "prepared_operation_impl.hpp"
#include "runtime/pump.hpp"
#include "runtime/test_access.hpp"
#include "scenario_support.hpp"
#include "scenarios.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <scry/scry.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scry::bench {
namespace {

constexpr auto admission_text = std::string_view{"snapshot admission"};

[[nodiscard]] ToolDefinition schema_definition(const std::size_t index,
                                               const std::size_t schema_bytes) {
  return {
      .name = "snapshot-tool-" + std::to_string(index),
      .description = fixture_text(128, index),
      .input_schema = Json{.text = representative_schema(schema_bytes, index)},
  };
}

[[nodiscard]] std::size_t definition_bytes(const ToolDefinition& definition) {
  return definition.name.size() + definition.description.size() +
         definition.input_schema.text.size();
}

[[nodiscard]] ToolHandler tool_handler() {
  return ToolHandler{[](Json input) -> Result<Json> { return input; }};
}

[[nodiscard]] Config schema_admission_config(const std::size_t pending_turns) {
  Config config{
      .base_url = "https://benchmark.invalid",
      .api_key = "benchmark-key",
      .model = "benchmark-model",
      .dialect = ProviderDialect::anthropic,
  };
  config.limits.max_pending_turns = pending_turns;
  config.retry.max_attempts = 1;
  return config;
}

[[nodiscard]] Error stopped_transport_error() {
  return {
      .category = ErrorCategory::cancelled,
      .message = "ownership benchmark transport stopped",
  };
}

struct ParkingTransportState final {
  void release() {
    std::lock_guard lock{mutex};
    released = true;
    changed.notify_all();
  }

  std::mutex mutex{};
  std::condition_variable_any changed{};
  bool released{};
};

class ParkingTransport final : public detail::Transport {
public:
  explicit ParkingTransport(std::shared_ptr<ParkingTransportState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] Result<detail::TransportResult>
  perform(const detail::TransportRequest&, const std::stop_token shutdown,
          const std::atomic<bool>& cancelled,
          detail::BodyChunkSink& body_sink) override {
    static_cast<void>(body_sink);
    std::unique_lock lock{state_->mutex};
    static_cast<void>(state_->changed.wait(lock, shutdown, [this, &cancelled] {
      return state_->released || cancelled.load(std::memory_order_acquire);
    }));
    return std::unexpected(stopped_transport_error());
  }

private:
  std::shared_ptr<ParkingTransportState> state_{};
};

[[nodiscard]] Result<Harness>
make_schema_harness(const std::shared_ptr<ParkingTransportState>& state,
                    const std::size_t pending_turns) {
  auto config = schema_admission_config(pending_turns);
  return detail::HarnessTestAccess::create(
      config, detail::make_provider_adapter(config.dialect),
      std::make_unique<ParkingTransport>(state));
}

[[nodiscard]] std::optional<Conversation> make_conversation(bool& valid) {
  auto created = Conversation::create();
  valid = valid && created.has_value();
  if (!created) {
    return std::nullopt;
  }
  return std::move(*created);
}

[[nodiscard]] detail::Message history_message(const std::size_t bytes,
                                              const std::size_t index) {
  return {
      .role = index % 2 == 0 ? detail::Role::user : detail::Role::assistant,
      .content = {detail::TextBlock{.text = fixture_text(bytes, index)}},
  };
}

[[nodiscard]] std::shared_ptr<detail::TurnRoute>
history_route(const std::shared_ptr<detail::CommandQueue>& commands,
              const std::shared_ptr<detail::ConversationState>& conversation) {
  return std::make_shared<detail::TurnRoute>(
      TurnId{.value = 1}, std::make_shared<std::atomic<bool>>(false), commands,
      conversation, "commit",
      detail::TurnRouteOptions{
          .max_conversation_bytes = std::size_t{64} * 1024 * 1024,
      });
}

[[nodiscard]] detail::CompletionEvent history_completion() {
  return {
      .turn_id = TurnId{.value = 1},
      .exchange = {detail::Message{
          .role = detail::Role::assistant,
          .content = {detail::TextBlock{.text = "completed"}},
      }},
      .finish_reason = FinishReason::completed,
      .attempt_count = 1,
  };
}

} // namespace

class SchemaAdmissionOperationState final {
public:
  SchemaAdmissionOperationState(const SchemaAdmissionShape operation_shape,
                                std::vector<ToolDefinition> schema_definitions,
                                const std::size_t logical_input_bytes,
                                const bool semantic_validation,
                                const ScenarioResult expected)
      : shape{operation_shape}, definitions{std::move(schema_definitions)},
        input_bytes{logical_input_bytes}, validate{semantic_validation},
        oracle{expected}, transport_state{std::make_shared<ParkingTransportState>()} {
    const auto pending_turns = std::max(std::size_t{4}, definitions.size() + 2);
    auto created = make_schema_harness(transport_state, pending_turns);
    setup_valid = created.has_value();
    if (!created) {
      return;
    }
    harness.emplace(std::move(*created));
    if (shape != SchemaAdmissionShape::retained_generations) {
      register_all();
    }
    prepare_target_conversations();
    if (shape == SchemaAdmissionShape::warm_accepted) {
      prepare_warm_turn();
    }
  }

  ~SchemaAdmissionOperationState() { transport_state->release(); }

  void register_all() {
    if (!harness) {
      return;
    }
    for (auto& definition : definitions) {
      setup_valid =
          harness->tools().add(std::move(definition), tool_handler()).has_value() &&
          setup_valid;
    }
  }

  void prepare_target_conversations() {
    const auto count = shape == SchemaAdmissionShape::retained_generations
                           ? definitions.size()
                           : std::size_t{1};
    conversations.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      auto conversation = make_conversation(setup_valid);
      if (conversation) {
        conversations.push_back(std::move(*conversation));
      }
    }
    turns.reserve(count);
  }

  void prepare_warm_turn() {
    warm_conversation = make_conversation(setup_valid);
    if (!warm_conversation || !harness) {
      return;
    }
    auto sent = harness->send(*warm_conversation, "warm snapshot");
    setup_valid = setup_valid && sent.has_value();
    if (sent) {
      warm_turn.emplace(std::move(*sent));
    }
    setup_valid =
        setup_valid && detail::HarnessTestAccess::has_current_tool_snapshot(*harness);
  }

  [[nodiscard]] bool rejected_send_is_valid(const std::expected<Turn, Error>& sent,
                                            const bool current_before) const {
    return !sent && sent.error().category == ErrorCategory::invalid_state &&
           !current_before &&
           !detail::HarnessTestAccess::has_current_tool_snapshot(*harness);
  }

  [[nodiscard]] bool accepted_send_is_valid(const std::expected<Turn, Error>& sent,
                                            const bool current_before) const {
    if (!sent || !detail::HarnessTestAccess::has_current_tool_snapshot(*harness)) {
      return false;
    }
    if (shape == SchemaAdmissionShape::cold_accepted) {
      return !current_before;
    }
    if (shape == SchemaAdmissionShape::warm_accepted) {
      return current_before;
    }
    return true;
  }

  void retain_accepted_turn(std::expected<Turn, Error>& sent) {
    if (sent) {
      turns.push_back(std::move(*sent));
    }
  }

  [[nodiscard]] std::uint64_t single_digest(const std::uint64_t turn_id) const {
    if (!validate) {
      return oracle.digest;
    }
    auto digest = fnv_offset;
    digest_number(digest, static_cast<std::uint64_t>(shape));
    digest_number(digest, definitions.size());
    digest_number(digest, turn_id);
    return digest;
  }

  [[nodiscard]] ScenarioResult run_single() {
    if (!harness || conversations.size() != 1) {
      return {};
    }
    const auto current_before =
        detail::HarnessTestAccess::has_current_tool_snapshot(*harness);
    auto sent =
        harness->send(conversations.front(), shape == SchemaAdmissionShape::rejected
                                                 ? std::string{}
                                                 : std::string{admission_text});
    const auto accepted_id = sent ? sent->id().value : 0;
    const auto admission_valid = shape == SchemaAdmissionShape::rejected
                                     ? rejected_send_is_valid(sent, current_before)
                                     : accepted_send_is_valid(sent, current_before);
    retain_accepted_turn(sent);
    return {
        .digest = single_digest(accepted_id),
        .input_bytes = static_cast<std::uint64_t>(input_bytes),
        .output_bytes = accepted_id != 0 ? sizeof(std::uint64_t) : 0,
        .items = 1,
        .valid = setup_valid && admission_valid && (validate || oracle.valid),
    };
  }

  [[nodiscard]] ScenarioResult run_retained_generations() {
    if (!harness || conversations.size() != definitions.size()) {
      return {};
    }
    auto valid = setup_valid;
    auto digest = validate ? fnv_offset : oracle.digest;
    std::size_t current_schema_bytes = 0;
    std::size_t retained_logical_bytes = 0;
    for (std::size_t index = 0; index < definitions.size(); ++index) {
      current_schema_bytes += definition_bytes(definitions[index]);
      valid = harness->tools()
                  .add(std::move(definitions[index]), tool_handler())
                  .has_value() &&
              valid;
      auto sent = harness->send(conversations[index], std::string{admission_text});
      valid = sent.has_value() &&
              detail::HarnessTestAccess::has_current_tool_snapshot(*harness) && valid;
      if (sent) {
        if (validate) {
          digest_number(digest, sent->id().value);
        }
        turns.push_back(std::move(*sent));
      }
      retained_logical_bytes += current_schema_bytes;
    }
    if (validate) {
      digest_number(digest, retained_logical_bytes);
    }
    return {
        .digest = digest,
        .input_bytes = static_cast<std::uint64_t>(input_bytes),
        .output_bytes = static_cast<std::uint64_t>(retained_logical_bytes),
        .items = static_cast<std::uint64_t>(turns.size()),
        .valid =
            valid && turns.size() == definitions.size() && (validate || oracle.valid),
    };
  }

  [[nodiscard]] ScenarioResult run() {
    return shape == SchemaAdmissionShape::retained_generations
               ? run_retained_generations()
               : run_single();
  }

private:
  SchemaAdmissionShape shape{};
  std::vector<ToolDefinition> definitions{};
  std::size_t input_bytes{};
  bool validate{};
  ScenarioResult oracle{};
  std::shared_ptr<ParkingTransportState> transport_state{};
  std::optional<Harness> harness{};
  std::optional<Conversation> warm_conversation{};
  std::optional<Turn> warm_turn{};
  std::vector<Conversation> conversations{};
  std::vector<Turn> turns{};
  bool setup_valid{true};
};

SchemaAdmissionScenario::SchemaAdmissionScenario(const SchemaAdmissionShape shape,
                                                 const std::size_t schema_count,
                                                 const std::size_t schema_bytes)
    : shape_(shape) {
  definitions_.reserve(schema_count);
  for (std::size_t index = 0; index < schema_count; ++index) {
    auto definition = schema_definition(index, schema_bytes);
    input_bytes_ += definition_bytes(definition);
    definitions_.push_back(std::move(definition));
  }
}

SchemaAdmissionOperation
SchemaAdmissionScenario::make_operation(const bool validate) const {
  return SchemaAdmissionOperation{std::make_unique<SchemaAdmissionOperationState>(
      shape_, definitions_, input_bytes_, validate, oracle_)};
}

ScenarioResult SchemaAdmissionScenario::validate() {
  auto operation = make_operation(true);
  oracle_ = operation.run();
  return oracle_;
}

SchemaAdmissionOperation SchemaAdmissionScenario::prepare() const {
  return make_operation(false);
}

class HistoryCommitOperationState final {
public:
  HistoryCommitOperationState(const HistoryCommitShape operation_shape,
                              const std::size_t message_count,
                              const std::size_t message_bytes,
                              const bool semantic_validation,
                              const ScenarioResult expected)
      : shape{operation_shape}, validate{semantic_validation}, oracle{expected},
        commands{std::make_shared<detail::CommandQueue>()},
        events{std::make_shared<detail::EventQueue>()}, pump{events},
        conversation{std::make_shared<detail::ConversationState>()} {
    conversation->messages->reserve(message_count + 2);
    for (std::size_t index = 0; index < message_count; ++index) {
      auto message = history_message(message_bytes, index);
      conversation->payload_bytes += detail::message_payload_bytes(message);
      conversation->messages->push_back(std::move(message));
    }
    original_block = conversation->messages.get();
    if (shape == HistoryCommitShape::aliased) {
      retained_snapshot = conversation->messages;
    }
    conversation->busy = true;
    route = history_route(commands, conversation);
    pump.add_route(route);
    setup_valid = events->push(history_completion(), 1024);
    expected_message_count = message_count;
    input_bytes = conversation->payload_bytes;
  }

  ~HistoryCommitOperationState() { pump.shutdown(); }

  [[nodiscard]] bool commit_is_valid(const UpdateStats& stats) const {
    const auto common_valid =
        setup_valid && stats.events_remaining == 0 && !stats.budget_exhausted &&
        route->terminal() && !conversation->busy &&
        conversation->messages->size() == expected_message_count + 2;
    if (shape == HistoryCommitShape::aliased) {
      return common_valid && retained_snapshot &&
             retained_snapshot->size() == expected_message_count &&
             conversation->messages.get() != original_block;
    }
    return common_valid && conversation->messages.get() == original_block;
  }

  [[nodiscard]] std::uint64_t result_digest() const {
    if (!validate) {
      return oracle.digest;
    }
    auto digest = fnv_offset;
    digest_number(digest, static_cast<std::uint64_t>(shape));
    digest_number(digest, expected_message_count);
    digest_number(digest, conversation->messages->size());
    digest_number(digest, conversation->payload_bytes);
    return digest;
  }

  [[nodiscard]] ScenarioResult run() {
    const auto stats = pump.update({});
    return {
        .digest = result_digest(),
        .input_bytes = static_cast<std::uint64_t>(input_bytes),
        .output_bytes = static_cast<std::uint64_t>(conversation->payload_bytes),
        .items = 1,
        .valid = commit_is_valid(stats) && (validate || oracle.valid),
    };
  }

private:
  HistoryCommitShape shape{};
  bool validate{};
  ScenarioResult oracle{};
  std::shared_ptr<detail::CommandQueue> commands{};
  std::shared_ptr<detail::EventQueue> events{};
  detail::PumpState pump;
  std::shared_ptr<detail::ConversationState> conversation{};
  std::shared_ptr<detail::TurnRoute> route{};
  std::shared_ptr<const std::vector<detail::Message>> retained_snapshot{};
  const std::vector<detail::Message>* original_block{};
  std::size_t expected_message_count{};
  std::size_t input_bytes{};
  bool setup_valid{};
};

HistoryCommitScenario::HistoryCommitScenario(const HistoryCommitShape shape,
                                             const std::size_t message_count,
                                             const std::size_t message_bytes)
    : shape_(shape), message_count_(message_count), message_bytes_(message_bytes) {}

HistoryCommitOperation
HistoryCommitScenario::make_operation(const bool validate) const {
  return HistoryCommitOperation{std::make_unique<HistoryCommitOperationState>(
      shape_, message_count_, message_bytes_, validate, oracle_)};
}

ScenarioResult HistoryCommitScenario::validate() {
  auto operation = make_operation(true);
  oracle_ = operation.run();
  return oracle_;
}

HistoryCommitOperation HistoryCommitScenario::prepare() const {
  return make_operation(false);
}

template class PreparedOperation<SchemaAdmissionOperationState>;
template class PreparedOperation<HistoryCommitOperationState>;

} // namespace scry::bench
