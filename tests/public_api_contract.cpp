#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

static_assert(std::is_aggregate_v<scry::Config>);
static_assert(std::is_aggregate_v<scry::HttpHeader>);
static_assert(std::is_enum_v<scry::ReasoningMode>);
static_assert(std::is_aggregate_v<scry::Error>);
static_assert(std::is_aggregate_v<scry::Json>);
static_assert(std::is_enum_v<scry::JsonKind>);
static_assert(std::is_default_constructible_v<scry::JsonView>);
static_assert(std::is_copy_constructible_v<scry::JsonView>);
static_assert(std::is_enum_v<scry::Role>);
static_assert(std::is_aggregate_v<scry::Message>);
static_assert(std::is_aggregate_v<scry::TextBlock>);
static_assert(std::is_aggregate_v<scry::ToolCallBlock>);
static_assert(std::is_aggregate_v<scry::ToolResultBlock>);
static_assert(
    std::same_as<scry::ContentBlock, std::variant<scry::TextBlock, scry::ToolCallBlock,
                                                  scry::ToolResultBlock>>);
static_assert(
    std::same_as<decltype(scry::Message::content), std::vector<scry::ContentBlock>>);
static_assert(std::is_aggregate_v<scry::ToolDefinition>);
static_assert(std::is_aggregate_v<scry::UpdateOptions>);
static_assert(std::is_aggregate_v<scry::TurnCallbacks>);

static_assert(std::is_enum_v<scry::FinishReason>);
static_assert(std::is_aggregate_v<scry::Usage>);
static_assert(std::is_aggregate_v<scry::ToolCall>);
static_assert(std::is_aggregate_v<scry::Completion>);
static_assert(std::is_aggregate_v<scry::UpdateStats>);
static_assert(std::is_aggregate_v<scry::ConversationConfig>);
static_assert(std::is_aggregate_v<scry::TurnId>);
static_assert(std::same_as<decltype(scry::Usage::input_tokens), std::uint64_t>);
static_assert(std::same_as<decltype(scry::Usage::output_tokens), std::uint64_t>);
static_assert(std::same_as<decltype(scry::ToolCall::turn_id), scry::TurnId>);
static_assert(std::same_as<decltype(scry::ToolCall::arguments), scry::Json>);
static_assert(std::same_as<decltype(scry::ToolCall::result), scry::Json>);
static_assert(std::same_as<decltype(scry::ToolCall::is_error), bool>);
static_assert(std::same_as<decltype(scry::Completion::turn_id), scry::TurnId>);
static_assert(
    std::same_as<decltype(scry::Completion::finish_reason), scry::FinishReason>);
static_assert(std::same_as<decltype(scry::Completion::usage), scry::Usage>);
static_assert(
    std::same_as<decltype(scry::UpdateStats::callbacks_delivered), std::size_t>);
static_assert(std::same_as<decltype(scry::UpdateStats::events_remaining), std::size_t>);
static_assert(std::same_as<decltype(scry::UpdateStats::budget_exhausted), bool>);
static_assert(
    std::same_as<decltype(scry::ConversationConfig::system_prompt), std::string>);
static_assert(std::same_as<decltype(scry::TurnId::value), std::uint64_t>);

// TurnId::operator bool is explicit and constexpr: zero is invalid, nonzero names
// an accepted turn.
constexpr scry::TurnId unset_turn{};
constexpr scry::TurnId set_turn{42};
static_assert(!unset_turn);
static_assert(bool{set_turn});

static_assert(std::is_move_constructible_v<scry::Conversation>);
static_assert(!std::is_copy_constructible_v<scry::Conversation>);
static_assert(!std::is_move_constructible_v<scry::ToolRegistry>);
static_assert(!std::is_move_assignable_v<scry::ToolRegistry>);
static_assert(!std::is_copy_constructible_v<scry::ToolRegistry>);
static_assert(std::is_move_constructible_v<scry::Turn>);
static_assert(!std::is_copy_constructible_v<scry::Turn>);
static_assert(std::is_move_constructible_v<scry::Harness>);
static_assert(!std::is_copy_constructible_v<scry::Harness>);
static_assert(std::is_move_constructible_v<scry::UniqueFunction<void()>>);
static_assert(!std::is_copy_constructible_v<scry::UniqueFunction<void()>>);

// A void-returning callback signature accepts a callable that returns something:
// the result is discarded rather than rejected at compile time.
using AppendingDelta = decltype([](std::string_view chunk) -> std::string& {
  static std::string sink;
  return sink.append(chunk);
});
static_assert(std::is_constructible_v<scry::TextDeltaCallback, AppendingDelta>);
static_assert(std::is_constructible_v<scry::UniqueFunction<void(std::string_view)>,
                                      AppendingDelta>);

// Callbacks are move-only and every member is optional, so a default-constructed
// TurnCallbacks is a valid "observe nothing" turn.
static_assert(std::is_default_constructible_v<scry::TurnCallbacks>);
static_assert(std::is_move_constructible_v<scry::TurnCallbacks>);
static_assert(!std::is_copy_constructible_v<scry::TurnCallbacks>);
static_assert(std::same_as<decltype(scry::TurnCallbacks::on_text_delta),
                           scry::TextDeltaCallback>);
static_assert(
    std::same_as<decltype(scry::TurnCallbacks::on_tool_call), scry::ToolCallCallback>);
static_assert(std::same_as<decltype(scry::TurnCallbacks::on_finished),
                           scry::UniqueFunction<void(scry::Result<scry::Completion>)>>);

// Turn is a cancellation handle only; callbacks are supplied to send(). The
// absence checks need a dependent type, or the missing member is a hard error.
template <typename T>
concept registers_completion =
    requires(T& turn) { turn.on_completion([](const scry::Completion&) {}); };

template <typename T>
concept registers_text_delta =
    requires(T& turn) { turn.on_text_delta([](std::string_view) {}); };

static_assert(requires(const scry::Turn& turn) {
  { turn.id() } -> std::same_as<scry::TurnId>;
});
static_assert(requires(scry::Turn& turn) {
  { turn.cancel() } -> std::same_as<bool>;
});
static_assert(requires(scry::Turn& turn) {
  { turn.disconnect() } -> std::same_as<bool>;
});
static_assert(!registers_completion<scry::Turn>);
static_assert(!registers_text_delta<scry::Turn>);
static_assert(requires(const scry::Turn& turn) {
  { turn.finished() } -> std::same_as<bool>;
});

// Thin queries over state the runtime already holds.
static_assert(requires(scry::Harness& harness) {
  { harness.cancel(scry::TurnId{}) } -> std::same_as<bool>;
  { harness.disconnect(scry::TurnId{}) } -> std::same_as<bool>;
});
static_assert(requires(const scry::Config& config) {
  { scry::Harness::validate(config) } -> std::same_as<scry::Status>;
});
static_assert(requires(const scry::ToolRegistry& registry) {
  { registry.contains(std::string_view{}) } -> std::same_as<bool>;
  { registry.names() } -> std::same_as<std::vector<std::string>>;
});
static_assert(requires(const scry::Conversation& conversation) {
  { conversation.messages() } -> std::same_as<const std::vector<scry::Message>&>;
  { conversation.system_prompt() } -> std::same_as<const std::string&>;
  { conversation.busy() } -> std::same_as<bool>;
});

// send() takes the callbacks atomically, and they are optional.
static_assert(requires(scry::Harness& harness, scry::Conversation& conversation) {
  {
    harness.send(conversation, std::string{})
  } -> std::same_as<scry::Result<scry::Turn>>;
  {
    harness.send(conversation, std::string{}, scry::TurnCallbacks{})
  } -> std::same_as<scry::Result<scry::Turn>>;
});

namespace {

bool move_only_callback_works() {
  bool callback_ran = false;
  scry::UniqueFunction<void()> source{[owned = std::make_unique<int>(7),
                                       &callback_ran] { callback_ran = *owned == 7; }};
  scry::UniqueFunction<void()> target;
  target = std::move(source);
  // UniqueFunction explicitly guarantees an empty, inspectable moved-from state.
  // NOLINTNEXTLINE(bugprone-use-after-move)
  if (source || !target) {
    return false;
  }

  auto moved = std::move(target);
  moved();
  moved.reset();
  if (!callback_ran || moved) {
    return false;
  }

  scry::UniqueFunction<int(int)> add_one{[](int value) { return value + 1; }};
  if (add_one(41) != 42) {
    return false;
  }

  try {
    moved();
  } catch (const std::bad_function_call&) {
    return true;
  }
  return false;
}

bool nonvoid_callable_in_void_signature_works() {
  std::string buffer;
  scry::UniqueFunction<void(std::string_view)> sink{
      [&buffer](std::string_view chunk) -> std::string& {
        return buffer.append(chunk);
      }};
  sink("hello ");
  sink("world");
  if (buffer != "hello world") {
    return false;
  }

  std::string via_callbacks;
  scry::TurnCallbacks callbacks{
      .on_text_delta = [&via_callbacks](std::string_view chunk) -> std::string& {
        return via_callbacks.append(chunk);
      },
  };
  callbacks.on_text_delta("delta");
  return via_callbacks == "delta";
}

struct Probe {
  void member() const {}
};

bool json_view_reads_a_parsed_document() {
  const auto document = scry::JsonView::parse(scry::Json{.text = R"({"ok":true})"});
  if (!document || document->kind() != scry::JsonKind::object ||
      document->size() != 1) {
    return false;
  }
  if (document->key_at(0) != "ok") {
    return false;
  }
  const auto member = document->find("ok");
  if (!member || member->boolean() != true) {
    return false;
  }
  if (scry::escape_json_string("a\"b\n") != "\"a\\\"b\\n\"") {
    return false;
  }
  return !scry::JsonView::parse(scry::Json{.text = "{"}).has_value();
}

bool null_pointer_callables_are_empty() {
  const scry::UniqueFunction<void()> from_null_function{
      static_cast<void (*)()>(nullptr)};
  if (from_null_function) {
    return false;
  }

  const scry::UniqueFunction<void(const Probe&)> from_null_member{
      static_cast<void (Probe::*)() const>(nullptr)};
  if (from_null_member) {
    return false;
  }

  auto empty = scry::UniqueFunction<void()>{static_cast<void (*)()>(nullptr)};
  bool threw = false;
  try {
    empty();
  } catch (const std::bad_function_call&) {
    threw = true;
  }
  if (!threw) {
    return false;
  }

  scry::UniqueFunction<void(const Probe&)> live_member{&Probe::member};
  if (!live_member) {
    return false;
  }
  live_member(Probe{});

  scry::UniqueFunction<int()> live_function{+[] { return 7; }};
  return static_cast<bool>(live_function) && live_function() == 7;
}

} // namespace

int main() {
  constexpr auto kibibyte = std::size_t{1024};
  const scry::Config config{
      .base_url = "http://localhost:8080",
      .model = "local-model",
      .dialect = scry::ProviderDialect::openai_compatible,
  };
  const auto default_checks = std::to_array<bool>({
      config.limits.max_pending_turns == 64,
      config.limits.max_sse_event_bytes == 256 * kibibyte,
      config.limits.max_response_bytes == 8 * kibibyte * kibibyte,
      config.limits.max_tool_arguments_bytes == kibibyte * kibibyte,
      config.limits.max_tool_result_bytes == 4 * kibibyte * kibibyte,
      config.limits.max_queued_event_bytes_per_turn == 2 * kibibyte * kibibyte,
      config.limits.max_conversation_bytes == 16 * kibibyte * kibibyte,
      config.max_tool_rounds == 8,
      config.sampling.max_tokens == 1024,
      config.reasoning_mode == scry::ReasoningMode::provider_default,
      config.retry.max_attempts == 3,
      config.retry.initial_backoff == std::chrono::milliseconds{250},
      config.retry.max_backoff == std::chrono::seconds{10},
      config.retry.max_elapsed == std::chrono::seconds{30},
      config.timeouts.connect == std::chrono::seconds{10},
      config.timeouts.idle == std::chrono::seconds{120},
      !config.timeouts.transfer.has_value(),
      config.timeouts.shutdown == std::chrono::seconds{2},
      config.tls_verify_peer,
      config.ca_bundle_path.empty(),
      config.proxy.empty(),
      config.extra_headers.empty(),
  });
  if (std::find(default_checks.begin(), default_checks.end(), false) !=
      default_checks.end()) {
    return 1;
  }

  const scry::Error error{
      .category = scry::ErrorCategory::resource_limit,
      .retryable = true,
      .attempt = 2,
      .message = "bounded",
  };
  if (!error.retryable || error.attempt != 2) {
    return 1;
  }

  if (!move_only_callback_works()) {
    return 1;
  }

  if (!nonvoid_callable_in_void_signature_works()) {
    return 1;
  }

  if (!null_pointer_callables_are_empty()) {
    return 1;
  }

  if (!json_view_reads_a_parsed_document()) {
    return 1;
  }

  static_assert(scry::version_major == SCRY_VERSION_MAJOR);
  static_assert(scry::version_minor == SCRY_VERSION_MINOR);
  static_assert(scry::version_patch == SCRY_VERSION_PATCH);
  static_assert(SCRY_VERSION == scry::version_major * 10000 +
                                    scry::version_minor * 100 + scry::version_patch);
  return scry::version == "0.3.0" ? 0 : 1;
}
