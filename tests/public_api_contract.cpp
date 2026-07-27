#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <scry/scry.hpp>
#include <type_traits>
#include <utility>

static_assert(std::is_aggregate_v<scry::Config>);
static_assert(std::is_aggregate_v<scry::Error>);
static_assert(std::is_aggregate_v<scry::Json>);
static_assert(std::is_aggregate_v<scry::ToolRegistrationOptions>);
static_assert(std::is_aggregate_v<scry::UpdateOptions>);
static_assert(scry::ToolRegistrationOptions{}.execution ==
              scry::ToolExecution::app_thread);

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
      config.retry.max_attempts == 3,
      config.retry.initial_backoff == std::chrono::milliseconds{250},
      config.retry.max_backoff == std::chrono::seconds{10},
      config.retry.max_elapsed == std::chrono::seconds{30},
      config.timeouts.connect == std::chrono::seconds{10},
      config.timeouts.transfer == std::chrono::seconds{120},
      config.timeouts.shutdown == std::chrono::seconds{2},
      config.tls_verify_peer,
  });
  if (std::find(default_checks.begin(), default_checks.end(), false) !=
      default_checks.end()) {
    return 1;
  }

  const scry::Error error{
      .category = scry::ErrorCategory::resource_limit,
      .message = "bounded",
  };
  if (error.retryable) {
    return 1;
  }

  if (!move_only_callback_works()) {
    return 1;
  }

  static_assert(scry::version_major == SCRY_VERSION_MAJOR);
  static_assert(scry::version_minor == SCRY_VERSION_MINOR);
  static_assert(scry::version_patch == SCRY_VERSION_PATCH);
  return scry::version == "0.0.1" ? 0 : 1;
}
