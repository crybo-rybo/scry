#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr std::string_view expected_answer = "E2E_SMOKE_OK";

// Only the lookup tool knows this code, so an answer that contains it shows that
// the tool result reached the model.
constexpr std::string_view lookup_code = "KESTREL-7319";

[[nodiscard]] std::string environment(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

[[nodiscard]] int report_error(std::string_view operation, const scry::Error& error) {
  std::cerr << operation << " failed: " << error.message << '\n';
  return 1;
}

[[nodiscard]] int report_failure(std::string_view step, std::string_view message) {
  std::cerr << step << ": " << message << '\n';
  return 1;
}

// The answer the typed scenario asks for. Every member is required, so the
// server has to honor the forced tool choice and fill the whole schema.
struct SmokeAnswer {
  [[= scry::reflection::description{
      "Copy this exactly: E2E_SMOKE_OK"}]] std::string token{};
  [[= scry::reflection::description{"The sum of 2 and 3"}]] std::int32_t sum{};
};

struct LookupArguments {
  [[= scry::reflection::description{"The name to look up"}]] std::string name{};
};

struct LookupResult {
  std::string code{};
};

// Reads the server, the model, and the dialect from the environment. Anthropic
// requires an API key. Scry rejects disabled reasoning for Anthropic, so that
// dialect keeps the provider default.
[[nodiscard]] std::expected<scry::Config, std::string> config_from_environment() {
  auto base_url = environment("SCRY_E2E_BASE_URL");
  auto model = environment("SCRY_E2E_MODEL");
  if (base_url.empty() || model.empty()) {
    return std::unexpected("Set SCRY_E2E_BASE_URL and SCRY_E2E_MODEL");
  }
  const auto dialect_name = environment("SCRY_E2E_DIALECT");
  if (!dialect_name.empty() && dialect_name != "openai" &&
      dialect_name != "anthropic") {
    return std::unexpected("SCRY_E2E_DIALECT must be openai or anthropic, not " +
                           dialect_name);
  }
  const bool anthropic = dialect_name == "anthropic";
  auto api_key = environment("SCRY_E2E_API_KEY");
  if (anthropic && api_key.empty()) {
    return std::unexpected("SCRY_E2E_DIALECT=anthropic requires SCRY_E2E_API_KEY");
  }
  return scry::Config{
      .base_url = std::move(base_url),
      .api_key = std::move(api_key),
      .model = std::move(model),
      .dialect = anthropic ? scry::ProviderDialect::anthropic
                           : scry::ProviderDialect::openai_compatible,
      .sampling =
          {
              .temperature = 0.0,
              .max_tokens = 512,
          },
      .reasoning_mode = anthropic ? scry::ReasoningMode::provider_default
                                  : scry::ReasoningMode::disabled,
      .retry =
          {
              .max_attempts = 1,
          },
      .timeouts =
          {
              .connect = std::chrono::seconds{5},
              .transfer = std::chrono::seconds{120},
              .shutdown = std::chrono::seconds{2},
          },
      .max_tool_rounds = 2,
  };
}

// A typed turn: the request forces a tool call, the model answers through the
// response tool, and ask<T>() decodes the answer.
[[nodiscard]] int run_typed_answer(scry::Harness& harness) {
  auto conversation = scry::Conversation::create({
      .system_prompt = "You are a deterministic protocol conformance agent. Answer "
                       "by calling the respond tool with the requested fields.",
  });
  if (!conversation) {
    return report_error("Conversation::create", conversation.error());
  }
  auto answered = harness.ask<SmokeAnswer>(
      *conversation, "Set token to E2E_SMOKE_OK and sum to the sum of 2 and 3.");
  if (!answered) {
    return report_error("Harness::ask", answered.error());
  }
  if (answered->value.token != expected_answer || answered->value.sum != 5) {
    return report_failure("Typed answer",
                          "unexpected answer " + answered->completion.structured->text);
  }
  std::cout << "Typed answer passed after " << answered->completion.answer_attempt_count
            << " attempt(s).\n";
  return 0;
}

// One tool round, streamed: send() accepts the turn and an update() loop delivers
// the text deltas, the reflected tool call, and the completion.
[[nodiscard]] int run_streamed_tool_turn(scry::Harness& harness,
                                         scry::Conversation& conversation) {
  std::string streamed;
  std::optional<scry::Result<scry::Completion>> finished;
  auto turn = harness.send(
      conversation,
      "Call e2e_lookup_code with name \"scry\" now. After its result, reply with "
      "only the code it returned.",
      {
          .on_text_delta = [&streamed](std::string_view delta) { streamed += delta; },
          .on_finished =
              [&finished](scry::Result<scry::Completion> result) {
                finished = std::move(result);
              },
      });
  if (!turn) {
    return report_error("Harness::send", turn.error());
  }
  while (!turn->finished()) {
    if (harness.update().callbacks_delivered == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }
  if (!finished || !*finished) {
    return finished ? report_error("Streamed tool turn", finished->error())
                    : report_failure("Streamed tool turn", "on_finished never ran");
  }
  const auto& completion = **finished;
  if (completion.tool_call_count != 1) {
    return report_failure("Streamed tool turn",
                          "expected one tool call, observed " +
                              std::to_string(completion.tool_call_count));
  }
  if (streamed.empty()) {
    return report_failure("Streamed tool turn", "on_text_delta received no text");
  }
  if (!completion.text.contains(lookup_code)) {
    return report_failure("Streamed tool turn",
                          "unexpected final text " + completion.text);
  }
  std::cout << "Streamed tool turn passed with " << streamed.size()
            << " streamed byte(s).\n";
  return 0;
}

// Restores the Conversation from its JSON document and sends a follow-up on the
// copy. The server must accept a history that holds a tool call and its result.
[[nodiscard]] int run_restored_follow_up(scry::Harness& harness,
                                         const scry::Conversation& conversation) {
  auto saved = conversation.to_json();
  if (!saved) {
    return report_error("Conversation::to_json", saved.error());
  }
  auto restored = scry::Conversation::from_json(*saved);
  if (!restored) {
    return report_error("Conversation::from_json", restored.error());
  }
  if (restored->message_count() != conversation.message_count()) {
    return report_failure("Restored conversation", "message count changed");
  }
  auto completion = harness.send_and_wait(
      *restored, "Do not call any tool. Repeat the code that e2e_lookup_code returned "
                 "earlier. Reply with only the code.");
  if (!completion) {
    return report_error("Harness::send_and_wait", completion.error());
  }
  if (completion->tool_call_count != 0) {
    return report_failure("Restored follow-up", "the model called a tool again");
  }
  if (!completion->text.contains(lookup_code)) {
    return report_failure("Restored follow-up",
                          "unexpected final text " + completion->text);
  }
  std::cout << "Restored follow-up passed over " << conversation.message_count()
            << " committed message(s).\n";
  return 0;
}

[[nodiscard]] int run_smoke() {
  auto config = config_from_environment();
  if (!config) {
    std::cerr << config.error() << '\n';
    return 2;
  }
  auto created = scry::Harness::create(std::move(*config));
  if (!created) {
    return report_error("Harness::create", created.error());
  }
  auto harness = std::move(*created);

  // The typed turn runs first, while the registry is empty, so the only tool the
  // model sees is the response tool.
  if (const int status = run_typed_answer(harness); status != 0) {
    return status;
  }

  auto registration = harness.tools().add<LookupArguments>(
      {
          .name = "e2e_lookup_code",
          .description = "Return the secret code for a name. The code cannot be "
                         "known without calling this tool.",
      },
      [](LookupArguments arguments) -> scry::Result<LookupResult> {
        if (arguments.name != "scry") {
          return std::unexpected(scry::tool_error("only the name scry has a code"));
        }
        return LookupResult{.code = std::string{lookup_code}};
      });
  if (!registration) {
    return report_error("ToolRegistry::add", registration.error());
  }

  auto conversation = scry::Conversation::create({
      .system_prompt = "You are a deterministic protocol conformance agent. Follow "
                       "the instructions exactly and answer briefly.",
  });
  if (!conversation) {
    return report_error("Conversation::create", conversation.error());
  }
  if (const int status = run_streamed_tool_turn(harness, *conversation); status != 0) {
    return status;
  }
  return run_restored_follow_up(harness, *conversation);
}

} // namespace

int main() { return run_smoke(); }
