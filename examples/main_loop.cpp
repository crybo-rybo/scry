#include <chrono>
#include <cstddef>
#include <iostream>
#include <scry/reflection.hpp>
#include <scry/scry.hpp>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

class Application {
public:
  [[nodiscard]] bool running() const noexcept { return !done_; }

  [[nodiscard]] std::string state_label() const {
    return done_ ? "main loop stopped" : "main loop running";
  }

  [[nodiscard]] bool loop_is_live() const noexcept { return !done_; }

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

// The reflected path is the flagship: the schema, the strict argument decode,
// and the result encode are all generated from these two aggregates. The
// annotation supplies the provider-visible property description.
struct StatusArguments {
  [[= scry::reflection::description{
      "Include a human-readable state label in the result"}]] bool verbose{false};
};

struct StatusResult {
  bool running{};
  std::string state{};
};

// Explicit-schema handlers own argument validation at the JSON boundary
// instead, and scry::JsonView reads the canonical arguments without a
// third-party parser.
[[nodiscard]] scry::Status validate_echo_arguments(const scry::JsonView& root) {
  const auto reject = [](std::string message) {
    return std::unexpected(scry::Error{
        .category = scry::ErrorCategory::tool,
        .message = std::move(message),
    });
  };
  if (root.kind() != scry::JsonKind::object) {
    return reject("echo expects a JSON object");
  }
  for (std::size_t index = 0; index < root.size(); ++index) {
    const auto key = root.key_at(index);
    if (!key || *key != "text") {
      return reject("echo accepts only the text property");
    }
  }
  return {};
}

[[nodiscard]] scry::ToolHandler echo_handler() {
  return [](const scry::Json& arguments) -> scry::Result<scry::Json> {
    // Every handler runs synchronously in harness.update() on this app thread.
    // Keep it bounded; long-running work needs an explicit deferred-result
    // contract rather than a background handler mode.
    //
    // Parse once and read from the one view. A parse failure yields the empty
    // view, which reports JsonKind::null and so fails the object check with the
    // same message a non-object root gets.
    const auto root = scry::JsonView::parse(arguments).value_or(scry::JsonView{});
    if (auto valid = validate_echo_arguments(root); !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    const auto text = root.find("text");
    if (!text || text->kind() != scry::JsonKind::string) {
      return std::unexpected(scry::Error{
          .category = scry::ErrorCategory::tool,
          .message = "echo requires a string text property",
      });
    }
    std::string result = R"({"echo":)";
    result += scry::escape_json_string(text->string().value_or(""));
    result += "}";
    return scry::Json{.text = std::move(result)};
  };
}

[[nodiscard]] scry::Status register_tools(scry::ToolRegistry& tools, Application& app) {
  // This tool is read-only. Side-effecting tools need an app-owned idempotency
  // key and reconciliation policy; see docs/architecture.md section 5.
  if (auto reflected = scry::reflection::add<StatusArguments>(
          tools,
          {
              .name = "get_application_status",
              .description =
                  "Report whether the host application's main loop is running",
          },
          [&app](StatusArguments arguments) {
            return StatusResult{
                .running = app.loop_is_live(),
                .state = arguments.verbose ? app.state_label() : "",
            };
          });
      !reflected) {
    return reflected;
  }
  return tools.add(
      scry::ToolDefinition{
          .name = "echo",
          .description = "Return the supplied text unchanged",
          .input_schema =
              {
                  .text =
                      R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})",
              },
      },
      echo_handler());
}

void print_block(const scry::ContentBlock& block) {
  std::visit(
      [](const auto& value) {
        using Block = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Block, scry::TextBlock>) {
          std::cout << "  text: " << value.text << '\n';
        } else if constexpr (std::is_same_v<Block, scry::ToolCallBlock>) {
          std::cout << "  tool call: " << value.name << '\n';
        } else {
          std::cout << "  tool result is_error: " << (value.is_error ? "true" : "false")
                    << '\n';
        }
      },
      block);
}

void print_history(const scry::Conversation& conversation) {
  std::cout << "conversation busy: " << (conversation.busy() ? "yes" : "no") << '\n';
  for (const auto& message : conversation.messages()) {
    std::cout << (message.role == scry::Role::user ? "user" : "assistant") << ":\n";
    for (const auto& block : message.content) {
      print_block(block);
    }
  }
}

// Reports the terminal outcome and stops the loop, either way.
[[nodiscard]] scry::TurnCallbacks loop_callbacks(Application& app) {
  return {
      .on_tool_call =
          [](const scry::ToolCall& call) {
            std::cout << "tool " << call.name
                      << (call.is_error ? " failed: " : " returned: ")
                      << call.result.text << '\n';
          },
      .on_finished =
          [&app](scry::Result<scry::Completion> finished) {
            if (finished) {
              app.show_answer(finished->text);
            } else {
              app.show_error(finished.error().message);
            }
          },
  };
}

} // namespace

int main() {
  // The Application outlives the Harness on purpose. The tool handlers and the
  // turn callbacks below capture it by reference, and a Harness delivers nothing
  // after its destructor begins, so the Harness must be destroyed first. Declaring
  // the app afterwards would leave those captures dangling during shutdown.
  Application app;

  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  const scry::Config config{
      .base_url = "http://127.0.0.1:11434/v1",
      .model = "qwen3:8b",
      .dialect = scry::ProviderDialect::openai_compatible,
      // Network options are plain Config fields. A corporate deployment would
      // also set `.proxy = "http://proxy.internal:3128"` and
      // `.ca_bundle_path = "/etc/ssl/certs/corporate.pem"`; both are left unset
      // here because a local Ollama needs neither.
      .extra_headers = {{.name = "x-scry-example", .value = "main-loop"}},
  };

  // Harness::validate runs exactly the create-time configuration checks without
  // starting libcurl or a worker, which is what a settings dialog wants. create()
  // can still fail afterwards for runtime reasons.
  if (const auto configured = scry::Harness::validate(config); !configured) {
    std::cerr << "invalid configuration: " << configured.error().message << '\n';
    return 1;
  }

  auto harness_result = scry::Harness::create(config);
  if (!harness_result) {
    std::cerr << harness_result.error().message << '\n';
    return 1;
  }
  auto harness = std::move(*harness_result);

  if (const auto registered = register_tools(harness.tools(), app); !registered) {
    std::cerr << registered.error().message << '\n';
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
  auto turn_result = harness.send(
      conversation, "Is the host application main loop running?", loop_callbacks(app));
  if (!turn_result) {
    std::cerr << turn_result.error().message << '\n';
    return 1;
  }
  // The handle only identifies, queries, and cancels. Keeping it lets the loop
  // stop on the turn's own terminal state as well as on the app's.
  const auto turn = std::move(*turn_result);

  // A real host calls update() once per existing frame tick. This standalone
  // example has no frame to piggyback on, so it sleeps when a pump delivered
  // nothing rather than spinning the core.
  while (app.running() && !turn.finished()) {
    const auto stats = harness.update({
        .time_budget = std::chrono::milliseconds{2},
        .max_callbacks = 32,
    });
    if (stats.callbacks_delivered == 0 && stats.events_remaining == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }

  print_history(conversation);

  return 0;
}
