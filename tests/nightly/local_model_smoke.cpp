#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view expected_answer = "NIGHTLY_SMOKE_OK";

[[nodiscard]] std::string required_environment(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    std::cerr << "Required environment variable is unset: " << name << '\n';
    return {};
  }
  return value;
}

[[nodiscard]] int report_error(std::string_view operation, const scry::Error& error) {
  std::cerr << operation << " failed: " << error.message << '\n';
  return 1;
}

// The answer the typed scenario asks for. Every member is required, so the
// server has to honor the forced tool choice and fill the whole schema.
struct SmokeAnswer {
  [[= scry::reflection::description{
      "Copy this exactly: NIGHTLY_SMOKE_OK"}]] std::string token{};
  [[= scry::reflection::description{"The sum of 2 and 3"}]] std::int32_t sum{};
};

// A typed turn over the live server: the request forces a tool call, the model
// answers through the response tool, and ask<T>() decodes the answer. It gets a
// Harness of its own, because the chat scenario's tool asks to be called before
// any final answer.
[[nodiscard]] int run_typed_answer(scry::Config config) {
  auto created = scry::Harness::create(std::move(config));
  if (!created) {
    return report_error("Harness::create", created.error());
  }
  auto harness = std::move(*created);
  auto conversation = scry::Conversation::create({
      .system_prompt = "You are a deterministic protocol conformance agent. Answer "
                       "by calling the respond tool with the requested fields.",
  });
  if (!conversation) {
    return report_error("Conversation::create", conversation.error());
  }
  auto answered = harness.ask<SmokeAnswer>(
      *conversation, "Set token to NIGHTLY_SMOKE_OK and sum to the sum of 2 and 3.");
  if (!answered) {
    return report_error("Harness::ask", answered.error());
  }
  if (answered->value.token != expected_answer || answered->value.sum != 5) {
    std::cerr << "Unexpected typed answer: " << answered->completion.structured->text
              << '\n';
    return 1;
  }
  std::cout << "Local-model typed answer passed after "
            << answered->completion.answer_attempt_count << " attempt(s).\n";
  return 0;
}

[[nodiscard]] int run_smoke() {
  auto base_url = required_environment("SCRY_LOCAL_MODEL_BASE_URL");
  auto model = required_environment("SCRY_LOCAL_MODEL_MODEL");
  if (base_url.empty() || model.empty()) {
    return 2;
  }
  const char* api_key = std::getenv("SCRY_LOCAL_MODEL_API_KEY");

  const auto config = scry::Config{
      .base_url = std::move(base_url),
      .api_key = api_key == nullptr ? std::string{} : std::string{api_key},
      .model = std::move(model),
      .dialect = scry::ProviderDialect::openai_compatible,
      .sampling =
          {
              .temperature = 0.0,
              .top_p = 1.0,
              .max_tokens = 512,
          },
      .reasoning_mode = scry::ReasoningMode::disabled,
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
  auto created = scry::Harness::create(config);
  if (!created) {
    return report_error("Harness::create", created.error());
  }
  auto harness = std::move(*created);

  int tool_call_count = 0;
  auto registration = harness.tools().add_dynamic(
      scry::ToolDefinition{
          .name = "nightly_required_check",
          .description =
              "Required conformance step. Call exactly once with no arguments "
              "before giving any final answer.",
          .input_schema =
              {
                  .text =
                      R"({"type":"object","properties":{},"additionalProperties":false})",
              },
      },
      [&tool_call_count](scry::Json arguments) -> scry::Result<scry::Json> {
        if (arguments.text != "{}") {
          return std::unexpected(scry::Error{
              .category = scry::ErrorCategory::tool,
              .message = "nightly_required_check expects an empty object",
          });
        }
        if (++tool_call_count != 1) {
          return std::unexpected(scry::Error{
              .category = scry::ErrorCategory::tool,
              .message = "nightly_required_check must be called exactly once",
          });
        }
        return scry::Json{
            .text =
                R"({"instruction":"Reply exactly NIGHTLY_SMOKE_OK.","status":"ready"})",
        };
      });
  if (!registration) {
    return report_error("ToolRegistry::add", registration.error());
  }

  auto conversation = scry::Conversation::create({
      .system_prompt =
          "You are a deterministic protocol conformance agent. Before any "
          "final answer, call nightly_required_check exactly once with {}. "
          "After the tool result, reply with exactly NIGHTLY_SMOKE_OK.",
  });
  if (!conversation) {
    return report_error("Conversation::create", conversation.error());
  }

  auto completion = harness.send_and_wait(
      *conversation,
      "Call nightly_required_check with {} now. After its result, give the exact "
      "final answer.");
  if (!completion) {
    return report_error("Harness::send_and_wait", completion.error());
  }
  if (tool_call_count != 1) {
    std::cerr << "Expected exactly one tool call, observed " << tool_call_count
              << ".\n";
    return 1;
  }
  constexpr std::string_view whitespace = " \n\r\t";
  const std::string_view text = completion->text;
  const auto first = text.find_first_not_of(whitespace);
  const auto trimmed =
      first == std::string_view::npos
          ? std::string_view{}
          : text.substr(first, text.find_last_not_of(whitespace) - first + 1);
  if (trimmed != expected_answer) {
    std::cerr << "Unexpected final text: " << completion->text << '\n';
    return 1;
  }
  std::cout << "Local-model chat and required tool round passed.\n";
  return run_typed_answer(config);
}

} // namespace

int main() { return run_smoke(); }
