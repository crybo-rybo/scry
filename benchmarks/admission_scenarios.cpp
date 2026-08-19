#include "core/provider.hpp"
#include "core/transport.hpp"
#include "runtime/test_access.hpp"
#include "scenarios.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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

constexpr std::uint64_t fnv_offset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t fnv_prime = 1'099'511'628'211ULL;
constexpr std::size_t admitted_turn_count = 63;

void digest_byte(std::uint64_t& digest, const std::uint8_t value) noexcept {
  digest ^= value;
  digest *= fnv_prime;
}

void digest_number(std::uint64_t& digest, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    digest_byte(digest, static_cast<std::uint8_t>(value & 0xffU));
    value >>= 8U;
  }
}

void digest_text(std::uint64_t& digest, const std::string_view text) noexcept {
  digest_number(digest, static_cast<std::uint64_t>(text.size()));
  for (const auto value : text) {
    digest_byte(digest, static_cast<std::uint8_t>(value));
  }
}

[[nodiscard]] std::string fixture_text(const std::size_t size, const std::size_t seed) {
  return std::string(size, static_cast<char>('a' + static_cast<char>(seed % 26)));
}

[[nodiscard]] std::string representative_schema(const std::size_t size,
                                                const std::size_t seed) {
  constexpr auto prefix = std::string_view{R"({"description":")"};
  constexpr auto suffix = std::string_view{
      R"(","properties":{"payload":{"type":"string"}},"type":"object"})"};
  const auto fixed_size = prefix.size() + suffix.size();
  if (size <= fixed_size) {
    return R"({"type":"object"})";
  }
  return std::string{prefix} + fixture_text(size - fixed_size, seed) +
         std::string{suffix};
}

[[nodiscard]] ToolDefinition tool_definition(const std::size_t index,
                                             const std::size_t schema_bytes) {
  return {
      .name = "benchmark-tool-" + std::to_string(index),
      .description = fixture_text(128, index),
      .input_schema = Json{.text = representative_schema(schema_bytes, index)},
  };
}

[[nodiscard]] ToolHandler tool_handler() {
  return ToolHandler{[](Json input) -> Result<Json> { return input; }};
}

[[nodiscard]] std::string history_document(const std::size_t message_count,
                                           const std::size_t message_bytes) {
  std::string document{R"({"messages":[)"};
  for (std::size_t index = 0; index < message_count; ++index) {
    if (index != 0) {
      document.push_back(',');
    }
    document.append(R"({"content":[{"text":")");
    document.append(fixture_text(message_bytes, index));
    document.append(R"(","type":"text"}],"role":")");
    document.append(index % 2 == 0 ? "user" : "assistant");
    document.append(R"("})");
  }
  document.append(R"(],"system_prompt":"","version":1})");
  return document;
}

[[nodiscard]] Config admission_config() {
  Config config{
      .base_url = "https://benchmark.invalid",
      .api_key = "benchmark-key",
      .model = "benchmark-model",
      .dialect = ProviderDialect::anthropic,
  };
  config.limits.max_pending_turns = admitted_turn_count + 1;
  config.retry.max_attempts = 1;
  return config;
}

void append_expected_history(detail::ModelRequest& request,
                             const std::size_t message_count,
                             const std::size_t message_bytes) {
  auto history = std::make_shared<std::vector<detail::Message>>();
  history->reserve(message_count);
  for (std::size_t index = 0; index < message_count; ++index) {
    history->push_back(detail::Message{
        .role = index % 2 == 0 ? detail::Role::user : detail::Role::assistant,
        .content = {detail::TextBlock{.text = fixture_text(message_bytes, index)}},
    });
  }
  request.history = std::move(history);
}

void append_expected_schemas(detail::ModelRequest& request,
                             const std::size_t schema_count,
                             const std::size_t schema_bytes) {
  auto schemas = std::make_shared<std::vector<detail::ToolSchema>>();
  schemas->reserve(schema_count);
  for (std::size_t index = 0; index < schema_count; ++index) {
    auto definition = tool_definition(index, schema_bytes);
    schemas->push_back(detail::ToolSchema{
        .name = std::move(definition.name),
        .description = std::move(definition.description),
        .input_schema = std::move(definition.input_schema),
    });
  }
  request.tools = std::move(schemas);
}

[[nodiscard]] std::optional<std::string>
expected_admission_body(const AdmissionShape shape, const std::size_t element_count,
                        const std::size_t element_bytes) {
  auto config = admission_config();
  detail::ModelRequest request{.sampling = config.sampling};
  if (shape == AdmissionShape::history) {
    append_expected_history(request, element_count, element_bytes);
  } else {
    append_expected_schemas(request, element_count, element_bytes);
  }
  request.messages.push_back(detail::Message{
      .role = detail::Role::user,
      .content = {detail::TextBlock{.text = "benchmark admission"}},
  });
  auto adapter = detail::make_provider_adapter(config.dialect);
  auto encoded = adapter->make_request(config, request);
  if (!encoded) {
    return std::nullopt;
  }
  return std::move(encoded->body);
}

[[nodiscard]] Error stopped_transport_error() {
  return {
      .category = ErrorCategory::cancelled,
      .message = "benchmark transport stopped",
  };
}

struct BlockingTransportState final {
  explicit BlockingTransportState(const bool capture) : capture_requests{capture} {
    if (capture_requests) {
      request_bodies.reserve(admitted_turn_count + 1);
    }
  }

  [[nodiscard]] bool wait_until_entered() {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, std::chrono::seconds{5}, [this] { return entered; });
  }

  [[nodiscard]] bool release_drain_and_validate(const std::string_view expected,
                                                std::uint64_t& digest) {
    std::unique_lock lock{mutex};
    released = true;
    changed.notify_all();
    const auto drained = changed.wait_for(lock, std::chrono::seconds{10}, [this] {
      return completed_requests == admitted_turn_count + 1;
    });
    if (!drained || request_bodies.size() != admitted_turn_count + 1) {
      return false;
    }
    auto valid = !request_bodies.front().empty();
    for (std::size_t index = 1; index < request_bodies.size(); ++index) {
      valid = request_bodies[index] == expected && valid;
      digest_text(digest, request_bodies[index]);
    }
    return valid;
  }

  std::mutex mutex{};
  std::condition_variable_any changed{};
  std::atomic<std::uint64_t> observed_bytes{};
  std::vector<std::string> request_bodies{};
  std::size_t completed_requests{};
  bool capture_requests{};
  bool entered{};
  bool released{};
};

class BlockingTransport final : public detail::Transport {
public:
  explicit BlockingTransport(std::shared_ptr<BlockingTransportState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] Result<detail::TransportResult>
  perform(const detail::TransportRequest& request, const std::stop_token shutdown,
          const std::atomic<bool>& cancelled,
          detail::BodyChunkSink& body_sink) override {
    static_cast<void>(body_sink);
    auto observed = request.url.size() + request.body.size();
    for (const auto& header : request.headers) {
      observed += header.name.size() + header.value.size();
    }
    state_->observed_bytes.fetch_add(observed, std::memory_order_relaxed);
    std::unique_lock lock{state_->mutex};
    state_->entered = true;
    state_->changed.notify_all();
    const auto released = state_->changed.wait(lock, shutdown, [this, &cancelled] {
      return state_->released || cancelled.load(std::memory_order_acquire);
    });
    if (!released || cancelled.load(std::memory_order_acquire)) {
      return std::unexpected(stopped_transport_error());
    }
    if (state_->capture_requests) {
      state_->request_bodies.push_back(request.body);
    }
    ++state_->completed_requests;
    state_->changed.notify_all();
    return std::unexpected(stopped_transport_error());
  }

private:
  std::shared_ptr<BlockingTransportState> state_{};
};

[[nodiscard]] std::vector<Conversation>
make_conversations(const AdmissionShape shape, const std::size_t element_count,
                   const std::size_t element_bytes, bool& valid) {
  std::vector<Conversation> conversations{};
  conversations.reserve(admitted_turn_count);
  const auto document = shape == AdmissionShape::history
                            ? history_document(element_count, element_bytes)
                            : std::string{};
  for (std::size_t index = 0; index < admitted_turn_count; ++index) {
    auto conversation = shape == AdmissionShape::history
                            ? Conversation::from_json(Json{.text = document})
                            : Conversation::create();
    if (!conversation) {
      valid = false;
      break;
    }
    conversations.push_back(std::move(*conversation));
  }
  return conversations;
}

[[nodiscard]] std::size_t register_admission_schemas(Harness& harness,
                                                     const std::size_t schema_count,
                                                     const std::size_t schema_bytes,
                                                     bool& valid) {
  std::size_t logical_bytes = 0;
  for (std::size_t index = 0; index < schema_count; ++index) {
    auto definition = tool_definition(index, schema_bytes);
    logical_bytes += definition.name.size() + definition.description.size() +
                     definition.input_schema.text.size();
    valid =
        valid && harness.tools().add(std::move(definition), tool_handler()).has_value();
  }
  valid = valid && harness.tools().size() == schema_count;
  return logical_bytes;
}

[[nodiscard]] Result<Harness>
make_admission_harness(const std::shared_ptr<BlockingTransportState>& state) {
  auto config = admission_config();
  return detail::HarnessTestAccess::create(
      config, detail::make_provider_adapter(config.dialect),
      std::make_unique<BlockingTransport>(state));
}

} // namespace

struct AdmissionOperation::Impl final {
  Impl(const AdmissionShape admission_shape, const std::size_t element_count,
       const std::size_t element_bytes, const bool semantic_validation,
       const ScenarioResult expected)
      : shape{admission_shape}, validate{semantic_validation}, oracle{expected},
        transport_state{std::make_shared<BlockingTransportState>(semantic_validation)},
        expected_element_count{element_count}, expected_element_bytes{element_bytes} {
    if (validate) {
      auto expected_body = expected_admission_body(shape, element_count, element_bytes);
      setup_valid = expected_body.has_value();
      if (expected_body) {
        expected_request_body = std::move(*expected_body);
      }
    }
    auto created = make_admission_harness(transport_state);
    setup_valid = created.has_value() && setup_valid;
    if (!created) {
      return;
    }
    harness.emplace(std::move(*created));
    if (shape == AdmissionShape::schemas) {
      per_turn_input_bytes = register_admission_schemas(*harness, element_count,
                                                        element_bytes, setup_valid);
    } else {
      per_turn_input_bytes = element_count * element_bytes;
    }
    conversations =
        make_conversations(shape, element_count, element_bytes, setup_valid);
    prepare_active_turn();
    messages.assign(admitted_turn_count, "benchmark admission");
    turns.reserve(admitted_turn_count);
  }

  void prepare_active_turn() {
    auto active_conversation_result = Conversation::create();
    setup_valid = setup_valid && active_conversation_result.has_value();
    if (!active_conversation_result || !harness) {
      return;
    }
    active_conversation.emplace(std::move(*active_conversation_result));
    auto sent = harness->send(*active_conversation, "park active turn");
    setup_valid = setup_valid && sent.has_value();
    if (!sent) {
      return;
    }
    active_turn.emplace(std::move(*sent));
    setup_valid = setup_valid && transport_state->wait_until_entered();
  }

  [[nodiscard]] bool validate_fixture(std::uint64_t& digest) const {
    auto valid = setup_valid && conversations.size() == admitted_turn_count;
    if (!validate) {
      return valid;
    }
    digest_number(digest, static_cast<std::uint64_t>(shape));
    digest_number(digest, expected_element_count);
    digest_number(digest, expected_element_bytes);
    digest_number(digest, harness->tools().size());
    const auto expected_messages =
        shape == AdmissionShape::history ? expected_element_count : std::size_t{0};
    for (const auto& conversation : conversations) {
      valid = conversation.message_count() == expected_messages && valid;
    }
    return valid;
  }

  [[nodiscard]] bool admit(const std::size_t index, std::uint64_t& digest) {
    auto sent = harness->send(conversations[index], std::move(messages[index]));
    if (!sent) {
      return false;
    }
    const auto turn_id = sent->id().value;
    if (validate) {
      digest_number(digest, turn_id);
    }
    turns.push_back(std::move(*sent));
    return turn_id == index + 2;
  }

  [[nodiscard]] bool drain_and_validate_requests(std::uint64_t& digest) {
    if (!validate) {
      return true;
    }
    return transport_state->release_drain_and_validate(expected_request_body, digest);
  }

  AdmissionShape shape{};
  bool validate{};
  ScenarioResult oracle{};
  std::shared_ptr<BlockingTransportState> transport_state{};
  std::optional<Harness> harness{};
  std::optional<Conversation> active_conversation{};
  std::optional<Turn> active_turn{};
  std::vector<Conversation> conversations{};
  std::vector<std::string> messages{};
  std::vector<Turn> turns{};
  std::string expected_request_body{};
  std::size_t per_turn_input_bytes{};
  std::size_t expected_element_count{};
  std::size_t expected_element_bytes{};
  bool setup_valid{true};
};

AdmissionOperation::AdmissionOperation(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

AdmissionOperation::~AdmissionOperation() = default;
AdmissionOperation::AdmissionOperation(AdmissionOperation&&) noexcept = default;
AdmissionOperation&
AdmissionOperation::operator=(AdmissionOperation&&) noexcept = default;

ScenarioResult AdmissionOperation::run() {
  if (!impl_ || !impl_->harness) {
    return {};
  }
  auto& operation = *impl_;
  auto digest = operation.validate ? fnv_offset : operation.oracle.digest;
  auto valid = operation.validate_fixture(digest);
  for (std::size_t index = 0; index < operation.conversations.size(); ++index) {
    valid = operation.admit(index, digest) && valid;
  }
  valid = operation.drain_and_validate_requests(digest) && valid;
  const auto user_message_bytes = std::string_view{"benchmark admission"}.size();
  return {
      .digest = digest,
      .input_bytes = static_cast<std::uint64_t>(
          admitted_turn_count * (operation.per_turn_input_bytes + user_message_bytes)),
      .output_bytes = admitted_turn_count * sizeof(std::uint64_t),
      .items = static_cast<std::uint64_t>(operation.turns.size()),
      .valid = valid && operation.turns.size() == admitted_turn_count &&
               (operation.validate || operation.oracle.valid),
  };
}

AdmissionScenario::AdmissionScenario(const AdmissionShape shape,
                                     const std::size_t element_count,
                                     const std::size_t element_bytes)
    : shape_(shape), element_count_(element_count), element_bytes_(element_bytes) {}

AdmissionOperation AdmissionScenario::make_operation(const bool validate) const {
  return AdmissionOperation{std::make_unique<AdmissionOperation::Impl>(
      shape_, element_count_, element_bytes_, validate, oracle_)};
}

ScenarioResult AdmissionScenario::validate() {
  auto operation = make_operation(true);
  oracle_ = operation.run();
  return oracle_;
}

AdmissionOperation AdmissionScenario::prepare() const { return make_operation(false); }

} // namespace scry::bench
