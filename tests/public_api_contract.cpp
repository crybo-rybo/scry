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

// A host may persist these values, so each enumerator keeps its number and a new
// reason, such as the soft stop's tool_round_limit, is appended at the end.
static_assert(std::to_underlying(scry::FinishReason::completed) == 0);
static_assert(std::to_underlying(scry::FinishReason::length) == 1);
static_assert(std::to_underlying(scry::FinishReason::tool_use) == 2);
static_assert(std::to_underlying(scry::FinishReason::unknown) == 3);
static_assert(std::to_underlying(scry::FinishReason::tool_round_limit) == 4);

// TurnId::operator bool is explicit and constexpr: zero is invalid, nonzero names
// an accepted turn.
constexpr scry::TurnId unset_turn{};
constexpr scry::TurnId set_turn{42};
static_assert(!unset_turn);
static_assert(bool{set_turn});

static_assert(std::is_move_constructible_v<scry::Conversation>);
static_assert(!std::is_copy_constructible_v<scry::Conversation>);
// A registry is a standalone value a host builds before any Harness exists, and
// hands to Harness::create() by move. Copying it would duplicate handlers.
static_assert(std::is_default_constructible_v<scry::ToolRegistry>);
static_assert(std::is_move_constructible_v<scry::ToolRegistry>);
static_assert(std::is_move_assignable_v<scry::ToolRegistry>);
static_assert(!std::is_copy_constructible_v<scry::ToolRegistry>);
static_assert(!std::is_copy_assignable_v<scry::ToolRegistry>);
static_assert(std::is_move_constructible_v<scry::Turn>);
static_assert(!std::is_copy_constructible_v<scry::Turn>);
static_assert(std::is_move_constructible_v<scry::Harness>);
static_assert(!std::is_copy_constructible_v<scry::Harness>);
static_assert(std::is_move_constructible_v<scry::UniqueFunction<void()>>);
static_assert(!std::is_copy_constructible_v<scry::UniqueFunction<void()>>);

// Callbacks are move-only and every member is optional, so a default-constructed
// TurnCallbacks is a valid "observe nothing" turn.
static_assert(std::is_default_constructible_v<scry::TurnCallbacks>);
static_assert(std::is_move_constructible_v<scry::TurnCallbacks>);
static_assert(!std::is_copy_constructible_v<scry::TurnCallbacks>);

// The two add_dynamic() overloads are separated by the handler's arity alone, so
// each lambda shape has to reach exactly one of them. A converting constructor that
// did not constrain on invocability would make both of these ambiguous.
using PlainToolLambda =
    decltype([](scrydantic::Json input) -> scry::Result<scrydantic::Json> {
      return input;
    });
using ContextualToolLambda =
    decltype([](const scry::ToolCallContext&,
                scrydantic::Json input) -> scry::Result<scrydantic::Json> {
      return input;
    });
static_assert(std::is_constructible_v<scry::ToolHandler, PlainToolLambda>);
static_assert(!std::is_constructible_v<scry::ToolHandler, ContextualToolLambda>);
static_assert(
    std::is_constructible_v<scry::ContextualToolHandler, ContextualToolLambda>);
static_assert(!std::is_constructible_v<scry::ContextualToolHandler, PlainToolLambda>);

static_assert(std::is_move_constructible_v<scry::ResponseFormat>);
static_assert(!std::is_copy_constructible_v<scry::ResponseFormat>);

// Typed sends take optional callbacks, as plain ones do.
namespace contract {
struct Verdict {
  bool supported{};
};
} // namespace contract
static_assert(requires(scry::Harness& harness, scry::Conversation& conversation) {
  {
    harness.send_structured(conversation, std::string{}, scry::ResponseFormat{},
                            scry::TurnCallbacks{})
  } -> std::same_as<scry::Result<scry::Turn>>;
  {
    harness.send<contract::Verdict>(conversation, std::string{})
  } -> std::same_as<scry::Result<scry::Turn>>;
});

// The structured sends have their own names, so `{}` as the third argument of
// send() names the callbacks and is not ambiguous.
static_assert(requires(scry::Harness& harness, scry::Conversation& conversation) {
  {
    harness.send(conversation, std::string{}, {})
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
      config.tool_round_limit == scry::ToolRoundLimitPolicy::fail,
      !config.max_tool_calls_per_turn.has_value(),
      config.sampling.max_tokens == 1024,
      !config.sampling.seed.has_value(),
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

  const auto refusal = scry::tool_error("north or south only");
  if (refusal.category != scry::ErrorCategory::tool ||
      refusal.model_message != "north or south only" ||
      refusal.message != "north or south only") {
    return 1;
  }
  const auto split = scry::tool_error("north or south only", "rejected move west");
  if (split.model_message != "north or south only" ||
      split.message != "rejected move west") {
    return 1;
  }
  const auto forwarded = scry::tool_error(scrydantic::Error{
      .message = "$.bed has no row in beds",
      .model_message = "$.bed must be a known bed",
      .issues = {{.kind = scrydantic::IssueKind::failed_check,
                  .path = {std::string{"bed"}},
                  .reason = "must be a known bed"}},
  });
  if (forwarded.category != scry::ErrorCategory::tool ||
      forwarded.message != "$.bed has no row in beds" ||
      forwarded.model_message != "$.bed must be a known bed" ||
      forwarded.issues.size() != 1) {
    return 1;
  }

  // The runtime checks that live in their own functions, run in one pass so
  // main stays within the complexity limit as the list grows.
  constexpr std::array runtime_checks{
      &move_only_callback_works,
      &nonvoid_callable_in_void_signature_works,
      &null_pointer_callables_are_empty,
  };
  if (!std::ranges::all_of(runtime_checks, [](const auto check) { return check(); })) {
    return 1;
  }

  return scry::version == "0.10.0" ? 0 : 1;
}
