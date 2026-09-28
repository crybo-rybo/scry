// End-to-end scenarios for the llamad dialect against a real daemon and model.
// Built only with SCRY_WITH_LLAMAD=ON, never registered with ctest: a live
// daemon is required, and some scenarios stop or restart it.
//
//   SCRY_LLAMAD_SOCKET=/tmp/llamad-e2e.sock ./scry_llamad_e2e <scenario>
//
// Scenario 12 runs $SCRY_E2E_RESTART_CMD between its turns, and scenario 13 runs
// $SCRY_E2E_KILL_CMD after the first streamed chunk. Each scenario prints what
// Scry reported and ends with PASS or FAIL against the documented behavior.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

int failures = 0;

void expect(const bool condition, const std::string_view what) {
  std::cout << (condition ? "  ok:   " : "  FAIL: ") << what << '\n';
  if (!condition) {
    ++failures;
  }
}

[[nodiscard]] long long elapsed_ms(const Clock::time_point since) {
  return std::chrono::duration_cast<milliseconds>(Clock::now() - since).count();
}

[[nodiscard]] std::string_view name(const scry::ErrorCategory category) {
  switch (category) {
  case scry::ErrorCategory::invalid_config:
    return "invalid_config";
  case scry::ErrorCategory::invalid_state:
    return "invalid_state";
  case scry::ErrorCategory::invalid_argument:
    return "invalid_argument";
  case scry::ErrorCategory::busy:
    return "busy";
  case scry::ErrorCategory::authentication:
    return "authentication";
  case scry::ErrorCategory::rate_limit:
    return "rate_limit";
  case scry::ErrorCategory::network:
    return "network";
  case scry::ErrorCategory::protocol:
    return "protocol";
  case scry::ErrorCategory::resource_limit:
    return "resource_limit";
  case scry::ErrorCategory::tool:
    return "tool";
  case scry::ErrorCategory::max_tool_rounds:
    return "max_tool_rounds";
  case scry::ErrorCategory::cancelled:
    return "cancelled";
  }
  return "?";
}

[[nodiscard]] std::string_view name(const scry::FinishReason reason) {
  switch (reason) {
  case scry::FinishReason::completed:
    return "completed";
  case scry::FinishReason::length:
    return "length";
  case scry::FinishReason::tool_use:
    return "tool_use";
  case scry::FinishReason::unknown:
    return "unknown";
  case scry::FinishReason::tool_round_limit:
    return "tool_round_limit";
  }
  return "?";
}

void print(const scry::Error& error) {
  std::cout << "  error: category=" << name(error.category)
            << " retryable=" << error.retryable << " attempt=" << error.attempt
            << " message=\"" << error.message << "\" provider_detail=\""
            << error.provider_detail << "\"\n";
}

void print(const scry::Completion& completion) {
  std::cout << "  completion: finish=" << name(completion.finish_reason)
            << " input_tokens=" << completion.usage.input_tokens
            << " output_tokens=" << completion.usage.output_tokens
            << " attempts=" << completion.attempt_count
            << " tool_rounds=" << completion.tool_round_count
            << " tool_calls=" << completion.tool_call_count
            << " rejected=" << completion.rejected_tool_call_count
            << " unexecuted=" << completion.unexecuted_tool_calls.size() << '\n'
            << "  text: " << completion.text << '\n';
}

void print(const scry::Result<scry::Completion>& result) {
  if (result) {
    print(*result);
  } else {
    print(result.error());
  }
}

void print_history(const scry::Conversation& conversation) {
  std::cout << "  history (" << conversation.message_count() << " messages):\n";
  for (const auto& message : conversation.messages()) {
    std::cout << "    " << (message.role == scry::Role::user ? "user" : "assistant")
              << ':';
    for (const auto& block : message.content) {
      if (const auto* text = std::get_if<scry::TextBlock>(&block)) {
        std::cout << " text(" << text->text.size() << "B)";
      } else if (const auto* call = std::get_if<scry::ToolCallBlock>(&block)) {
        std::cout << " tool_call(" << call->name << ' ' << call->arguments.text << ')';
      } else if (const auto* result = std::get_if<scry::ToolResultBlock>(&block)) {
        std::cout << " tool_result(" << (result->is_error ? "error " : "")
                  << result->result.text << ')';
      }
    }
    std::cout << '\n';
  }
}

[[nodiscard]] std::string socket_target() {
  const char* socket = std::getenv("SCRY_LLAMAD_SOCKET");
  if (socket == nullptr || *socket == '\0') {
    std::cerr << "SCRY_LLAMAD_SOCKET is unset\n";
    std::exit(2);
  }
  return std::string{"unix:"} + socket;
}

[[nodiscard]] scry::Config base_config() {
  return {
      .base_url = socket_target(),
      .model = "local",
      .dialect = scry::ProviderDialect::llamad,
      .sampling = {.temperature = 0.0, .max_tokens = 1024},
  };
}

[[nodiscard]] scry::Harness make_harness(scry::Config config) {
  auto created = scry::Harness::create(std::move(config));
  if (!created) {
    print(created.error());
    std::exit(1);
  }
  return std::move(*created);
}

[[nodiscard]] scry::Conversation make_conversation(std::string system_prompt = {}) {
  auto created =
      scry::Conversation::create({.system_prompt = std::move(system_prompt)});
  if (!created) {
    print(created.error());
    std::exit(1);
  }
  return std::move(*created);
}

void run_command(const char* variable) {
  const char* command = std::getenv(variable);
  if (command == nullptr || *command == '\0') {
    std::cerr << variable << " is unset\n";
    std::exit(2);
  }
  std::cout << "  running " << variable << '\n';
  if (std::system(command) != 0) {
    std::cerr << variable << " failed\n";
    std::exit(2);
  }
}

// A generation long enough to cancel, time out, or interrupt mid-stream.
constexpr std::string_view long_generation_prompt =
    "Write a detailed 800-word story about a lighthouse keeper.";

struct TurnOutcome {
  std::optional<scry::Result<scry::Completion>> result;
  std::size_t delta_count{};
  Clock::time_point first_delta{};
  Clock::time_point finished{};
};

// Sends one turn and pumps until it finishes, running `on_first_delta` once
// when the first text arrives.
TurnOutcome run_turn(scry::Harness& harness, scry::Conversation& conversation,
                     std::string message,
                     std::function<void(scry::Turn&)> on_first_delta = {}) {
  TurnOutcome outcome;
  std::optional<scry::Turn> turn;
  auto sent = harness.send(conversation, std::move(message),
                           {
                               .on_text_delta =
                                   [&](std::string_view) {
                                     if (outcome.delta_count++ == 0) {
                                       outcome.first_delta = Clock::now();
                                       if (on_first_delta) {
                                         on_first_delta(*turn);
                                       }
                                     }
                                   },
                               .on_finished =
                                   [&](scry::Result<scry::Completion> result) {
                                     outcome.finished = Clock::now();
                                     outcome.result = std::move(result);
                                   },
                           });
  if (!sent) {
    outcome.result = std::unexpected(sent.error());
    return outcome;
  }
  turn.emplace(std::move(*sent));
  while (!outcome.result) {
    harness.update();
    std::this_thread::sleep_for(milliseconds{1});
  }
  return outcome;
}

struct TimeArgs {
  [[= scry::reflection::description{
      "IANA time zone, such as Europe/Oslo"}]] std::string timezone;
};

struct TimeReading {
  std::string timezone;
  std::string time;
};

void add_clock(scry::Harness& harness) {
  const auto added = scry::reflection::add<TimeArgs>(
      harness.tools(),
      {.name = "get_current_time",
       .description = "Get the current time in a time zone"},
      [](TimeArgs args) {
        return TimeReading{.timezone = std::move(args.timezone),
                           .time = "2026-09-28 22:19:45 CEST"};
      });
  if (!added) {
    print(added.error());
    std::exit(1);
  }
}

// --- scenarios -------------------------------------------------------------

void plain_chat() {
  auto harness = make_harness(base_config());
  auto conversation = make_conversation();
  const auto outcome = run_turn(harness, conversation, "Hello");
  print(*outcome.result);
  std::cout << "  deltas=" << outcome.delta_count << '\n';
  expect(outcome.result->has_value(), "turn completed");
  if (*outcome.result) {
    const auto& completion = **outcome.result;
    expect(completion.finish_reason == scry::FinishReason::completed,
           "finish completed");
    expect(completion.usage.input_tokens > 0 && completion.usage.output_tokens > 0,
           "usage non-zero");
    expect(completion.attempt_count == 1, "one attempt");
    expect(outcome.delta_count > 1, "text streamed in several deltas");
  }
}

void multi_turn() {
  auto harness = make_harness(base_config());
  auto conversation = make_conversation("You are a concise assistant. /no_think");
  for (const auto* message : {"My name is Ada. Reply with one short sentence.",
                              "What is 2 + 3?", "What is my name?"}) {
    const auto outcome = run_turn(harness, conversation, message);
    print(*outcome.result);
    expect(outcome.result->has_value(), "turn completed");
  }
  print_history(conversation);
  expect(conversation.message_count() == 6, "six committed messages");
}

void tool_one_round() {
  auto harness = make_harness(base_config());
  add_clock(harness);
  auto conversation = make_conversation();
  std::size_t observed = 0;
  std::optional<scry::Result<scry::Completion>> result;
  auto sent = harness.send(conversation, "What time is it in Oslo?",
                           {
                               .on_tool_call =
                                   [&](const scry::ToolCall& call) {
                                     ++observed;
                                     std::cout << "  on_tool_call: " << call.name << ' '
                                               << call.arguments.text << " -> "
                                               << call.result.text << '\n';
                                   },
                               .on_finished =
                                   [&](scry::Result<scry::Completion> finished) {
                                     result = std::move(finished);
                                   },
                           });
  while (sent && !result) {
    harness.update();
    std::this_thread::sleep_for(milliseconds{1});
  }
  print(*result);
  print_history(conversation);
  expect(result->has_value(), "turn completed");
  if (*result) {
    expect(observed == 1, "one on_tool_call");
    expect((*result)->tool_round_count == 1, "tool_round_count 1");
    expect((*result)->text.find("22:19") != std::string::npos,
           "answer uses the result");
  }
}

void tool_error_then_success() {
  auto harness = make_harness(base_config());
  int calls = 0;
  const auto added = scry::reflection::add<TimeArgs>(
      harness.tools(),
      {.name = "get_current_time",
       .description = "Get the current time in a time zone"},
      [&calls](TimeArgs args) -> scry::Result<TimeReading> {
        if (++calls == 1) {
          return std::unexpected(
              scry::tool_error("The time service is briefly unavailable. Call the "
                               "tool again with the same arguments."));
        }
        return TimeReading{.timezone = std::move(args.timezone),
                           .time = "2026-09-28 22:19:45 CEST"};
      });
  if (!added) {
    print(added.error());
    std::exit(1);
  }
  auto conversation = make_conversation();
  const auto outcome = run_turn(harness, conversation, "What time is it in Oslo?");
  print(*outcome.result);
  print_history(conversation);
  expect(outcome.result->has_value(), "turn completed");
  if (*outcome.result) {
    expect(calls == 2, "tool called twice");
    expect((*outcome.result)->tool_round_count == 2, "two tool rounds");
    expect((*outcome.result)->rejected_tool_call_count == 0, "nothing rejected");
  }
}

struct CityArgs {
  [[= scry::reflection::description{"City name, such as Oslo"}]] std::string city;
};
struct CityCode {
  std::string code;
};
struct CodeArgs {
  [[= scry::reflection::description{
      "City code returned by lookup_city_code"}]] std::string code;
};
struct Weather {
  std::string summary;
};

void tool_round_limit() {
  auto config = base_config();
  config.max_tool_rounds = 1;
  config.tool_round_limit = scry::ToolRoundLimitPolicy::complete;
  auto harness = make_harness(std::move(config));
  auto added = scry::reflection::add<CityArgs>(
      harness.tools(),
      {.name = "lookup_city_code",
       .description = "Look up the opaque city code that get_weather requires"},
      [](CityArgs) { return CityCode{.code = "C-4471"}; });
  auto added_weather = scry::reflection::add<CodeArgs>(
      harness.tools(),
      {.name = "get_weather",
       .description = "Get the weather for a city code from lookup_city_code"},
      [](CodeArgs) { return Weather{.summary = "Light rain, 9 C"}; });
  if (!added || !added_weather) {
    std::exit(1);
  }
  auto conversation = make_conversation();
  const auto outcome =
      run_turn(harness, conversation,
               "What is the weather in Oslo? You must first look up its city code, "
               "then call get_weather with that code.");
  print(*outcome.result);
  print_history(conversation);
  expect(outcome.result->has_value(), "turn completed");
  if (*outcome.result) {
    const auto& completion = **outcome.result;
    expect(completion.finish_reason == scry::FinishReason::tool_round_limit,
           "finish tool_round_limit");
    expect(!completion.unexecuted_tool_calls.empty(), "unexecuted calls reported");
    for (const auto& call : completion.unexecuted_tool_calls) {
      std::cout << "  unexecuted: " << call.name << ' ' << call.arguments.text << '\n';
    }
  }
}

void small_max_tokens() {
  auto config = base_config();
  config.sampling.max_tokens = 8;
  auto harness = make_harness(std::move(config));
  auto conversation = make_conversation();
  const auto outcome = run_turn(harness, conversation, "Write a poem about the sea.");
  print(*outcome.result);
  expect(outcome.result->has_value() &&
             (*outcome.result)->finish_reason == scry::FinishReason::length,
         "finish length");
}

void cancel_mid_stream() {
  auto harness = make_harness(base_config());
  auto conversation = make_conversation();
  Clock::time_point cancelled_at{};
  const auto outcome =
      run_turn(harness, conversation, std::string{long_generation_prompt},
               [&](scry::Turn& turn) {
                 cancelled_at = Clock::now();
                 turn.cancel();
               });
  print(*outcome.result);
  const auto latency =
      std::chrono::duration_cast<milliseconds>(outcome.finished - cancelled_at);
  std::cout << "  cancel -> on_finished: " << latency.count() << " ms\n";
  expect(!outcome.result->has_value() &&
             outcome.result->error().category == scry::ErrorCategory::cancelled,
         "cancelled");
  expect(conversation.message_count() == 0, "history unchanged");
  expect(latency < milliseconds{2000}, "well under timeouts.shutdown");
}

void destroy_mid_stream() {
  std::optional<scry::Harness> harness{make_harness(base_config())};
  auto conversation = make_conversation();
  bool streaming = false;
  auto sent =
      harness->send(conversation, std::string{long_generation_prompt},
                    {.on_text_delta = [&](std::string_view) { streaming = true; }});
  if (!sent) {
    print(sent.error());
    std::exit(1);
  }
  while (!streaming) {
    harness->update();
    std::this_thread::sleep_for(milliseconds{1});
  }
  const auto start = Clock::now();
  harness.reset();
  const auto took = elapsed_ms(start);
  std::cout << "  ~Harness took " << took << " ms\n";
  expect(took < 2500, "destructor returns within about timeouts.shutdown");
  expect(!conversation.busy(), "conversation not busy");
}

[[nodiscard]] std::string long_prompt() {
  std::string text = "Here is a log. Reply with only the number of its last line.\n";
  for (int line = 1; line <= 300; ++line) {
    text += "Line " + std::to_string(line) +
            ": the quick brown fox jumps over the lazy dog near the river bank.\n";
  }
  return text;
}

void idle_during_prefill() {
  auto config = base_config();
  config.timeouts.idle = milliseconds{500};
  config.retry.max_attempts = 2;
  auto harness = make_harness(std::move(config));
  auto conversation = make_conversation("/no_think");
  auto start = Clock::now();
  const auto short_idle = run_turn(harness, conversation, long_prompt());
  std::cout << "  short idle, " << elapsed_ms(start) << " ms:\n";
  print(*short_idle.result);
  expect(!short_idle.result->has_value() &&
             short_idle.result->error().category == scry::ErrorCategory::network &&
             short_idle.result->error().provider_detail == "llamad:idle_timeout" &&
             short_idle.result->error().retryable,
         "network llamad:idle_timeout, retryable");
  expect(!short_idle.result->has_value() && short_idle.result->error().attempt == 2,
         "second attempt also failed");

  auto patient = make_harness(base_config());
  start = Clock::now();
  const auto default_idle = run_turn(patient, conversation, long_prompt());
  std::cout << "  default idle, " << elapsed_ms(start) << " ms:\n";
  print(*default_idle.result);
  expect(default_idle.result->has_value(), "default idle succeeds");
}

void transfer_deadline() {
  auto config = base_config();
  config.timeouts.transfer = milliseconds{3000};
  auto harness = make_harness(std::move(config));
  auto conversation = make_conversation();
  const auto start = Clock::now();
  const auto outcome =
      run_turn(harness, conversation, std::string{long_generation_prompt});
  std::cout << "  took " << elapsed_ms(start) << " ms, deltas=" << outcome.delta_count
            << '\n';
  print(*outcome.result);
  expect(!outcome.result->has_value() &&
             outcome.result->error().category == scry::ErrorCategory::network &&
             outcome.result->error().provider_detail == "llamad:deadline_exceeded",
         "network llamad:deadline_exceeded");
}

void daemon_not_running() {
  auto config = base_config();
  config.base_url = "unix:/tmp/scry-e2e-nobody.sock";
  auto harness = make_harness(std::move(config));
  auto conversation = make_conversation();
  const auto start = Clock::now();
  const auto outcome = run_turn(harness, conversation, "Hello");
  std::cout << "  took " << elapsed_ms(start)
            << " ms (3 attempts, 250/500 ms backoff)\n";
  print(*outcome.result);
  expect(!outcome.result->has_value() &&
             outcome.result->error().category == scry::ErrorCategory::network &&
             outcome.result->error().message == "llamad daemon is unreachable",
         "network, daemon is unreachable");
  expect(!outcome.result->has_value() && outcome.result->error().attempt == 3,
         "retried per RetryPolicy");
}

void restart_between_turns(const milliseconds settle) {
  auto harness = make_harness(base_config());
  auto conversation = make_conversation("/no_think");
  const auto first = run_turn(harness, conversation, "Say hi.");
  print(*first.result);
  run_command("SCRY_E2E_RESTART_CMD");
  std::this_thread::sleep_for(settle);
  const auto start = Clock::now();
  const auto second = run_turn(harness, conversation, "Say bye.");
  std::cout << "  second turn after " << settle.count() << " ms settle, took "
            << elapsed_ms(start) << " ms\n";
  print(*second.result);
  expect(first.result->has_value(), "first turn completed");
  expect(second.result->has_value(), "second turn completed after restart");
}

// A turn fails while the daemon is down, then the next is sent the moment the
// daemon listens again, inside the channel's reconnect backoff.
void restart_after_failed_turn() {
  auto harness = make_harness(base_config());
  auto conversation = make_conversation("/no_think");
  run_command("SCRY_E2E_KILL_CMD");
  const auto down = run_turn(harness, conversation, "Say hi.");
  print(*down.result);
  run_command("SCRY_E2E_START_CMD");
  const auto start = Clock::now();
  const auto back = run_turn(harness, conversation, "Say hi.");
  std::cout << "  immediate turn took " << elapsed_ms(start) << " ms\n";
  print(*back.result);
  expect(!down.result->has_value(), "turn fails while the daemon is down");
}

void killed_mid_stream() {
  auto harness = make_harness(base_config());
  auto conversation = make_conversation();
  const auto outcome =
      run_turn(harness, conversation, std::string{long_generation_prompt},
               [](scry::Turn&) { run_command("SCRY_E2E_KILL_CMD"); });
  print(*outcome.result);
  expect(!outcome.result->has_value() &&
             outcome.result->error().category == scry::ErrorCategory::network &&
             outcome.result->error().retryable,
         "retryable network error");
  expect(!outcome.result->has_value() && outcome.result->error().attempt == 1,
         "not retried after streamed text");
  expect(conversation.message_count() == 0, "history unchanged");
}

void oversized_request() {
  auto harness = make_harness(base_config());
  auto conversation = make_conversation();
  const auto outcome = run_turn(harness, conversation,
                                std::string(std::size_t{4} * 1024 * 1024 + 4096, 'a'));
  print(*outcome.result);
  expect(!outcome.result->has_value() &&
             outcome.result->error().category == scry::ErrorCategory::resource_limit &&
             outcome.result->error().provider_detail == "llamad:resource_exhausted",
         "resource_limit llamad:resource_exhausted");
}

void context_overflow() {
  auto harness = make_harness(base_config());
  auto conversation = make_conversation();
  std::string text;
  for (int word = 0; word < 12000; ++word) {
    text += "word" + std::to_string(word) + ' ';
  }
  std::cout << "  prompt bytes=" << text.size() << '\n';
  const auto outcome = run_turn(harness, conversation, std::move(text));
  print(*outcome.result);
  expect(!outcome.result->has_value() &&
             outcome.result->error().category == scry::ErrorCategory::protocol &&
             outcome.result->error().message == "llamad rejected the request" &&
             outcome.result->error().provider_detail == "llamad:invalid_argument",
         "protocol llamad:invalid_argument");
}

void two_conversations() {
  auto harness = make_harness(base_config());
  auto first = make_conversation("/no_think");
  auto second = make_conversation("/no_think");
  std::vector<std::string> order;
  std::map<std::string, bool> succeeded;
  auto callbacks = [&](std::string label) {
    return scry::TurnCallbacks{
        .on_finished =
            [&, label](scry::Result<scry::Completion> result) {
              order.push_back(label);
              succeeded[label] = result.has_value();
              std::cout << "  " << label << ':';
              print(result);
            },
    };
  };
  auto turn_a = harness.send(first, "Name a colour.", callbacks("A"));
  auto turn_b = harness.send(second, "Name a fruit.", callbacks("B"));
  if (!turn_a || !turn_b) {
    std::exit(1);
  }
  while (order.size() < 2) {
    harness.update();
    std::this_thread::sleep_for(milliseconds{1});
  }
  expect(order == std::vector<std::string>{"A", "B"}, "FIFO order");
  expect(succeeded["A"] && succeeded["B"], "both completed");
}

} // namespace

int main(const int argc, char** argv) {
  const std::map<std::string, std::function<void()>, std::less<>> scenarios{
      {"1", plain_chat},
      {"2", multi_turn},
      {"3", tool_one_round},
      {"4", tool_error_then_success},
      {"5", tool_round_limit},
      {"6", small_max_tokens},
      {"7", cancel_mid_stream},
      {"8", destroy_mid_stream},
      {"9", idle_during_prefill},
      {"10", transfer_deadline},
      {"11", daemon_not_running},
      {"12", [] { restart_between_turns(milliseconds{2000}); }},
      {"12b", [] { restart_between_turns(milliseconds{0}); }},
      {"12c", restart_after_failed_turn},
      {"13", killed_mid_stream},
      {"14", oversized_request},
      {"15", context_overflow},
      {"16", two_conversations},
  };
  const auto found =
      argc == 2 ? scenarios.find(std::string_view{argv[1]}) : scenarios.end();
  if (found == scenarios.end()) {
    std::cerr << "usage: " << argv[0] << " <scenario 1..16, 12b>\n";
    return 2;
  }
  std::cout << "scenario " << found->first << '\n';
  found->second();
  std::cout << (failures == 0 ? "PASS" : "FAIL") << '\n';
  return failures == 0 ? 0 : 1;
}
