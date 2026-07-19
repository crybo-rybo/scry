#include <chrono>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <utility>

namespace {

class Application {
public:
  [[nodiscard]] bool running() const noexcept { return !done_; }

  [[nodiscard]] std::string status_json() const {
    return done_ ? R"({"running":false})" : R"({"running":true})";
  }

  void show_answer(const std::string& answer) {
    std::cout << answer << '\n';
    done_ = true;
  }

  void show_error(const std::string& message) {
    std::cerr << message << '\n';
    done_ = true;
  }

private:
  bool done_{false};
};

} // namespace

int main() {
  auto harness_result = scry::Harness::create(scry::Config{
      .base_url = "https://api.anthropic.com",
      .api_key = "load-from-the-host-secret-store",
      .model = "configured-by-the-host",
  });
  if (!harness_result) {
    std::cerr << harness_result.error().message << '\n';
    return 1;
  }
  auto harness = std::move(*harness_result);

  Application app;
  auto registration = harness.tools().add(
      scry::ToolDefinition{
          .name = "get_application_status",
          .description = "Report whether the host application's main loop is running",
          .input_schema =
              {
                  .text =
                      R"({"type":"object","properties":{},"additionalProperties":false})",
              },
      },
      [&app](scry::Json arguments) -> scry::Result<scry::Json> {
        // Explicit-schema handlers own argument validation at the C++23 boundary.
        if (arguments.text != "{}") {
          return std::unexpected(scry::Error{
              .category = scry::ErrorCategory::tool,
              .message = "get_application_status expects an empty object",
          });
        }
        // This tool is read-only. Side-effecting tools need an app-owned
        // idempotency key and reconciliation policy; see DESIGN.md section 8.
        return scry::Json{.text = app.status_json()};
      },
      scry::ToolRegistrationOptions{
          .execution = scry::ToolExecution::app_thread,
      });
  if (!registration) {
    std::cerr << registration.error().message << '\n';
    return 1;
  }

  auto conversation_result = scry::Conversation::create({
      .system_prompt = "Answer briefly and use tools when useful.",
  });
  if (!conversation_result) {
    std::cerr << conversation_result.error().message << '\n';
    return 1;
  }
  auto conversation = std::move(*conversation_result);

  auto turn_result =
      harness.send(conversation, "Is the host application main loop running?");
  if (!turn_result) {
    std::cerr << turn_result.error().message << '\n';
    return 1;
  }
  auto turn = std::move(*turn_result);
  auto callback_status = turn.on_complete(
      [&app](const scry::Completion& completion) { app.show_answer(completion.text); });
  if (!callback_status) {
    std::cerr << callback_status.error().message << '\n';
    return 1;
  }
  callback_status = turn.on_error(
      [&app](const scry::Error& error) { app.show_error(error.message); });
  if (!callback_status) {
    std::cerr << callback_status.error().message << '\n';
    return 1;
  }
  callback_status = turn.on_cancelled(
      [&app](const scry::Cancelled&) { app.show_error("Turn cancelled"); });
  if (!callback_status) {
    std::cerr << callback_status.error().message << '\n';
    return 1;
  }

  while (app.running()) {
    harness.update({
        .time_budget = std::chrono::milliseconds{2},
        .max_callbacks = 32,
    });
  }

  return 0;
}
