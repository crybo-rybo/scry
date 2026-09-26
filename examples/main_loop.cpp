#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

class Application {
public:
  [[nodiscard]] bool running() const noexcept { return !done_; }

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

// The schema, the strict argument decode, and the result encode are all generated
// from these two aggregates. The annotation supplies the property description the
// model sees.
struct StatusArguments {
  [[= scry::reflection::description{
      "Include a human-readable state label in the result"}]] bool verbose{false};
};

struct StatusResult {
  bool running{};
  std::string state{};
};

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

// The explicit-schema path: the handler owns validation at the JSON boundary.
[[nodiscard]] scry::ToolHandler echo_handler() {
  return [](const scry::Json& arguments) -> scry::Result<scry::Json> {
    // Every handler runs synchronously inside harness.update(), on this thread.
    // Keep it bounded; there is no background handler mode.
    //
    // A parse failure yields the empty view, which reports JsonKind::null and so
    // fails the object check with the same message a non-object root gets.
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

// Registers one reflected tool and one explicit-schema tool.
[[nodiscard]] scry::Status register_tools(scry::ToolRegistry& tools, Application& app) {
  if (auto reflected = scry::reflection::add<StatusArguments>(
          tools,
          {
              .name = "get_application_status",
              .description =
                  "Report whether the host application's main loop is running",
          },
          // A reflected handler may take the call's identity as an optional
          // leading parameter; its string views are borrowed for this call only.
          [&app](const scry::ToolCallContext& context, StatusArguments arguments) {
            std::cout << "tool " << context.tool_name << " in round " << context.round
                      << '\n';
            const bool running = app.running();
            const auto* label = running ? "main loop running" : "main loop stopped";
            return StatusResult{
                .running = running,
                .state = arguments.verbose ? label : "",
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

[[nodiscard]] int write_stdout(std::string_view text, std::string_view what) {
  std::cout << text << '\n';
  std::cout.flush();
  if (!std::cout) {
    std::cerr << "Failed to write " << what << " to stdout\n";
    return 1;
  }
  return 0;
}

[[nodiscard]] int print_tool_manifest(const scry::ToolRegistry& tools) {
  const auto manifest = tools.to_json();
  if (!manifest) {
    std::cerr << manifest.error().message << '\n';
    return 1;
  }
  return write_stdout(manifest->text, "tool manifest");
}

// Result schemas never reach the model; exporting one lets a host publish the full
// tool contract next to the manifest.
[[nodiscard]] int print_result_schema() {
  return write_stdout(scry::reflection::schema_v<StatusResult>, "result schema");
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

// Admission runs before every handler, after Config::max_tool_calls_per_turn has
// had its say. Refusing costs the model an answer, not the turn: it sees
// {"error": model_message} and keeps going. The same shape gives "accept this
// result, then stop": set a flag from on_tool_call or from a handler and refuse
// everything afterwards. Cancelling cannot do that, because it rolls the turn
// back and discards the results the executed tools produced; whatever those
// handlers already changed outside the conversation is left for the host to
// reconcile.
[[nodiscard]] scry::ToolAdmissionCallback echo_once_per_turn() {
  using Verdict = std::optional<scry::ToolRejection>;
  return [echo_calls = 0](const scry::ToolRequest& request) mutable -> Verdict {
    if (request.context.tool_name != "echo" || echo_calls++ == 0) {
      return std::nullopt;
    }
    return scry::ToolRejection{
        .model_message =
            "echo may be called once per turn; answer with what you already have",
    };
  };
}

// Under ToolRoundLimitPolicy::complete the round limit ends the turn rather than
// failing it, so the answer is real and what is missing is only whatever the
// dropped calls would have added.
void print_completion(const scry::Completion& completion) {
  std::cout << "tools used: " << completion.tool_call_count << '\n';
  if (completion.finish_reason == scry::FinishReason::tool_round_limit) {
    std::cout << "tool rounds exhausted; " << completion.unexecuted_tool_calls.size()
              << " requested calls never ran\n";
  }
}

// on_finished runs exactly once: with the completion, or with the terminal error
// (including a cancelled one), unless harness destruction begins first.
[[nodiscard]] scry::TurnCallbacks loop_callbacks(Application& app) {
  return {
      .on_tool_request = echo_once_per_turn(),
      .on_tool_call =
          [](const scry::ToolCall& call) {
            std::cout << "tool " << call.name
                      << (call.is_error ? " failed: " : " returned: ")
                      << call.result.text << '\n';
          },
      .on_finished =
          [&app](scry::Result<scry::Completion> finished) {
            if (finished) {
              print_completion(*finished);
              app.show_answer(finished->text);
            } else {
              app.show_error(finished.error().message);
            }
          },
  };
}

// The two export modes build only the registry, so they exit before any
// provider configuration or worker exists.
enum class Mode : std::uint8_t {
  run,
  tool_manifest,
  result_schema,
  usage,
};

[[nodiscard]] Mode parse_mode(const int argc, char* argv[]) {
  if (argc == 1) {
    return Mode::run;
  }
  const auto flag = argc == 2 ? std::string_view{argv[1]} : std::string_view{};
  if (flag == "--tool-manifest") {
    return Mode::tool_manifest;
  }
  if (flag == "--result-schema") {
    return Mode::result_schema;
  }
  return Mode::usage;
}

} // namespace

int main(int argc, char* argv[]) {
  const auto mode = parse_mode(argc, argv);
  if (mode == Mode::usage) {
    std::cerr << "Usage: " << (argc > 0 ? argv[0] : "scry_canonical_example")
              << " [--tool-manifest | --result-schema]\n";
    return 1;
  }
  // Declared before the harness on purpose: the tool handlers and turn callbacks
  // capture it by reference, and the harness must be destroyed first so those
  // captures cannot dangle during shutdown.
  Application app;

  // A registry is a plain value, so the tool contract can be built and exported
  // with no provider configuration, no libcurl, and no worker thread.
  scry::ToolRegistry tools;
  if (const auto registered = register_tools(tools, app); !registered) {
    std::cerr << registered.error().message << '\n';
    return 1;
  }

  if (mode == Mode::tool_manifest) {
    return print_tool_manifest(tools);
  }
  if (mode == Mode::result_schema) {
    return print_result_schema();
  }

  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  const scry::Config config{
      .base_url = "http://127.0.0.1:11434/v1",
      .model = "qwen3:8b",
      .dialect = scry::ProviderDialect::openai_compatible,
      // Stop at the round limit instead of failing: the tools that ran already
      // changed this application's state, and rolling the turn back would not undo
      // them.
      .tool_round_limit = scry::ToolRoundLimitPolicy::complete,
      // A corporate deployment would also set `.proxy` and `.ca_bundle_path`.
      .extra_headers = {{.name = "x-scry-example", .value = "main-loop"}},
  };

  // validate() runs the create-time configuration checks without starting libcurl
  // or a worker, which is what a settings dialog wants.
  if (const auto configured = scry::Harness::validate(config); !configured) {
    std::cerr << "invalid configuration: " << configured.error().message << '\n';
    return 1;
  }

  // The harness adopts the registry; `tools` is inactive from here on, and any
  // further registration goes through harness.tools().
  auto harness_result = scry::Harness::create(config, std::move(tools));
  if (!harness_result) {
    std::cerr << harness_result.error().message << '\n';
    return 1;
  }
  auto harness = std::move(*harness_result);

  auto conversation_result = scry::Conversation::create({
      .system_prompt = "Answer briefly and use tools when useful.",
  });
  if (!conversation_result) {
    std::cerr << conversation_result.error().message << '\n';
    return 1;
  }
  auto conversation = std::move(*conversation_result);

  // Callbacks travel with the send, so nothing can be missed between acceptance and
  // the first update().
  auto turn_result = harness.send(
      conversation, "Is the host application main loop running?", loop_callbacks(app));
  if (!turn_result) {
    std::cerr << turn_result.error().message << '\n';
    return 1;
  }
  const auto turn = std::move(*turn_result);

  // A real host calls update() once per existing frame tick. This example has no
  // frame to piggyback on, so it sleeps when a pump delivered nothing rather than
  // spinning a core.
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
