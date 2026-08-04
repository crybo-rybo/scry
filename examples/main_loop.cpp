#include <chrono>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <thread>
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
  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  auto harness_result = scry::Harness::create(scry::Config{
      .base_url = "http://127.0.0.1:11434/v1",
      .model = "qwen3:8b",
      .dialect = scry::ProviderDialect::openai_compatible,
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
        // Every handler runs synchronously in harness.update() on this app
        // thread. Keep it bounded; future long-running work needs an explicit
        // deferred-result contract rather than a background handler mode.
        // Explicit-schema handlers own argument validation at the C++23 boundary.
        // Exact-text comparison only holds for this zero-argument schema; tools
        // with real arguments parse the JSON (or use the reflected overload in
        // examples/reflection_tools.cpp, which decodes into typed structs).
        if (arguments.text != "{}") {
          return std::unexpected(scry::Error{
              .category = scry::ErrorCategory::tool,
              .message = "get_application_status expects an empty object",
          });
        }
        // This tool is read-only. Side-effecting tools need an app-owned
        // idempotency key and reconciliation policy; see DESIGN.md section 8.
        return scry::Json{.text = app.status_json()};
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

  // Callbacks travel with the send, so nothing can be missed between acceptance and
  // the first update(). This non-empty on_finished runs exactly once: with the
  // completion, or with the terminal error, including an ErrorCategory::cancelled one,
  // unless Harness destruction begins first.
  //
  // The returned handle only identifies and cancels. This example keeps it only long
  // enough to inspect the admission result and never uses the accepted handle again;
  // callbacks remain route-owned even after a handle is dropped.
  const auto turn =
      harness.send(conversation, "Is the host application main loop running?",
                   {
                       .on_finished =
                           [&app](scry::Result<scry::Completion> finished) {
                             if (finished) {
                               app.show_answer(finished->text);
                             } else {
                               app.show_error(finished.error().message);
                             }
                           },
                   });
  if (!turn) {
    std::cerr << turn.error().message << '\n';
    return 1;
  }

  // A real host calls update() once per existing frame tick. This standalone
  // example has no frame to piggyback on, so it sleeps when a pump delivered
  // nothing rather than spinning the core.
  while (app.running()) {
    const auto stats = harness.update({
        .time_budget = std::chrono::milliseconds{2},
        .max_callbacks = 32,
    });
    if (stats.callbacks_delivered == 0 && stats.events_remaining == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }

  return 0;
}
