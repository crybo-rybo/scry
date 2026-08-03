#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <format>
#include <glaze/glaze.hpp>
#include <iostream>
#include <memory>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace scry_tool_chat {

constexpr std::string_view default_base_url = "http://127.0.0.1:11434/v1";
constexpr std::string_view default_model = "qwen3:8b";

struct BinaryArguments {
  std::optional<double> a{};
  std::optional<double> b{};
};

struct ArithmeticResult {
  double result{};
};

[[nodiscard]] std::string environment_or(const char* name,
                                         const std::string_view fallback) {
  const char* value = std::getenv(name);
  return value == nullptr || *value == '\0' ? std::string{fallback}
                                            : std::string{value};
}

[[nodiscard]] std::string optional_environment(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

[[nodiscard]] scry::Error tool_error(std::string message) {
  return scry::Error{
      .category = scry::ErrorCategory::tool,
      .message = std::move(message),
  };
}

class ToolActivity final {
public:
  [[nodiscard]] scry::Result<scry::Json> observe(const std::string_view name,
                                                 scry::Result<scry::Json> result) {
    pending_ = Record{
        .name = std::string{name},
        .result = result ? result->text : result.error().message,
        .succeeded = result.has_value(),
    };
    return result;
  }

  void show(const scry::ToolCall& call) {
    std::cout << "tool> " << call.name << ' ' << call.arguments.text << '\n';
    if (pending_ && pending_->name == call.name) {
      std::cout << (pending_->succeeded ? "result> " : "error> ") << pending_->result
                << '\n';
    } else {
      std::cout << "result> unavailable\n";
    }
    pending_.reset();
  }

private:
  struct Record {
    std::string name{};
    std::string result{};
    bool succeeded{};
  };

  std::optional<Record> pending_{};
};

[[nodiscard]] scry::Result<scry::Json> current_datetime(const scry::Json& arguments) {
  if (arguments.text != "{}") {
    return std::unexpected(tool_error("get_current_datetime expects {}"));
  }

  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  if (gmtime_r(&seconds, &utc) == nullptr) {
    return std::unexpected(tool_error("current UTC time is unavailable"));
  }

  std::array<char, 32> timestamp{};
  if (std::strftime(timestamp.data(), timestamp.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) ==
      0) {
    return std::unexpected(tool_error("current UTC time could not be formatted"));
  }
  return scry::Json{
      .text = std::format(R"({{"datetime":"{}","timezone":"UTC"}})", timestamp.data())};
}

using ArithmeticOperation = double (*)(double, double);

[[nodiscard]] scry::Result<scry::Json> calculate(const scry::Json& arguments,
                                                 const ArithmeticOperation operation) {
  BinaryArguments parsed{};
  if (glz::read_json(parsed, arguments.text) || !parsed.a || !parsed.b) {
    return std::unexpected(
        tool_error("arithmetic tools require finite numeric a and b fields"));
  }
  if (!std::isfinite(*parsed.a) || !std::isfinite(*parsed.b)) {
    return std::unexpected(
        tool_error("arithmetic tools require finite numeric a and b fields"));
  }

  const double value = operation(*parsed.a, *parsed.b);
  if (!std::isfinite(value)) {
    return std::unexpected(tool_error("arithmetic result is not finite"));
  }
  auto encoded = glz::write_json(ArithmeticResult{.result = value});
  if (!encoded) {
    return std::unexpected(tool_error("arithmetic result could not be encoded"));
  }
  return scry::Json{.text = std::move(*encoded)};
}

[[nodiscard]] double add(const double a, const double b) noexcept { return a + b; }

[[nodiscard]] double multiply(const double a, const double b) noexcept { return a * b; }

[[nodiscard]] scry::Status register_datetime_tool(scry::Harness& harness,
                                                  ToolActivity& activity) {
  return harness.tools().add(
      scry::ToolDefinition{
          .name = "get_current_datetime",
          .description =
              "Return the host's current date and time in UTC. Use this instead "
              "of estimating the current date or time.",
          .input_schema =
              {
                  .text =
                      R"({"type":"object","properties":{},"additionalProperties":false})",
              },
      },
      [&activity](scry::Json arguments) -> scry::Result<scry::Json> {
        auto result = current_datetime(arguments);
        return activity.observe("get_current_datetime", std::move(result));
      });
}

[[nodiscard]] scry::Status register_arithmetic_tool(
    scry::Harness& harness, ToolActivity& activity, const std::string_view name,
    const std::string_view description, const ArithmeticOperation operation) {
  std::string observed_name{name};
  return harness.tools().add(
      scry::ToolDefinition{
          .name = observed_name,
          .description = std::string{description},
          .input_schema =
              {
                  .text =
                      R"({"type":"object","properties":{"a":{"type":"number","description":"First operand"},"b":{"type":"number","description":"Second operand"}},"required":["a","b"],"additionalProperties":false})",
              },
      },
      [&activity, observed_name = std::move(observed_name),
       operation](scry::Json arguments) -> scry::Result<scry::Json> {
        auto result = calculate(arguments, operation);
        return activity.observe(observed_name, std::move(result));
      });
}

[[nodiscard]] scry::Status register_tools(scry::Harness& harness,
                                          ToolActivity& activity) {
  if (auto status = register_datetime_tool(harness, activity); !status) {
    return status;
  }
  if (auto status = register_arithmetic_tool(
          harness, activity, "add",
          "Add two numbers. Use this tool for addition instead of mental arithmetic.",
          add);
      !status) {
    return status;
  }
  return register_arithmetic_tool(
      harness, activity, "multiply",
      "Multiply two numbers. Use this tool for multiplication instead of mental "
      "arithmetic.",
      multiply);
}

class TurnDisplay final {
public:
  [[nodiscard]] bool running() const noexcept { return running_; }

  void show_delta(const std::string_view delta) {
    if (!assistant_line_open_) {
      std::cout << "assistant> ";
      assistant_line_open_ = true;
    }
    received_delta_ = true;
    std::cout << delta << std::flush;
  }

  void show_tool(const scry::ToolCall& call, ToolActivity& activity) {
    finish_assistant_line();
    activity.show(call);
  }

  void show_completion(const scry::Completion& completion) {
    finish_assistant_line();
    if (!received_delta_) {
      std::cout << "assistant> " << completion.text << '\n';
    }
    std::cout << std::format("[turn completed in {} model request{}]\n",
                             completion.attempt_count,
                             completion.attempt_count == 1 ? "" : "s");
    running_ = false;
  }

  void show_error(const std::string_view message) {
    finish_assistant_line();
    std::cerr << "error> " << message << '\n';
    running_ = false;
  }

private:
  void finish_assistant_line() {
    if (assistant_line_open_) {
      std::cout << '\n';
      assistant_line_open_ = false;
    }
  }

  bool running_{true};
  bool assistant_line_open_{};
  bool received_delta_{};
};

[[nodiscard]] bool attach_callbacks(scry::Turn& turn, TurnDisplay& display,
                                    ToolActivity& activity) {
  auto status = turn.on_text_delta(
      [&display](const std::string_view delta) { display.show_delta(delta); });
  if (status) {
    status = turn.on_tool_call([&display, &activity](const scry::ToolCall& call) {
      display.show_tool(call, activity);
    });
  }
  if (status) {
    status = turn.on_completion([&display](const scry::Completion& completion) {
      display.show_completion(completion);
    });
  }
  if (status) {
    status = turn.on_error(
        [&display](const scry::Error& error) { display.show_error(error.message); });
  }
  if (status) {
    status = turn.on_cancelled(
        [&display](const scry::Cancelled&) { display.show_error("Turn cancelled"); });
  }
  if (!status) {
    std::cerr << "Callback registration failed: " << status.error().message << '\n';
  }
  return status.has_value();
}

void pump_until_terminal(scry::Harness& harness, const TurnDisplay& display) {
  while (display.running()) {
    const auto stats = harness.update({
        .time_budget = std::chrono::milliseconds{2},
        .max_callbacks = 32,
    });
    if (stats.callbacks_delivered == 0 && stats.events_remaining == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }
}

[[nodiscard]] scry::Result<scry::Harness> create_harness(const std::string& base_url,
                                                         const std::string& model) {
  return scry::Harness::create({
      .base_url = base_url,
      .api_key = optional_environment("SCRY_LOCAL_MODEL_API_KEY"),
      .model = model,
      .dialect = scry::ProviderDialect::openai_compatible,
      .sampling = {.temperature = 0.0, .max_tokens = 1024},
      .max_tool_rounds = 8,
  });
}

[[nodiscard]] scry::Result<scry::Conversation> create_conversation() {
  return scry::Conversation::create({
      .system_prompt =
          "You are a terminal assistant with tools. Use get_current_datetime for "
          "current date or time questions. Use add and multiply for arithmetic "
          "instead of calculating mentally. If the user explicitly asks you to "
          "call a tool, you must call it. Never claim a tool was called unless you "
          "actually invoked it. Explain results concisely.",
  });
}

void show_help() {
  std::cout << "Commands:\n"
               "  /help   Show this help\n"
               "  /tools  List available tools\n"
               "  /reset  Start a new conversation\n"
               "  /quit   Exit\n";
}

void show_tools() {
  std::cout << "Tools:\n"
               "  get_current_datetime({})\n"
               "  add({\"a\": number, \"b\": number})\n"
               "  multiply({\"a\": number, \"b\": number})\n";
}

enum class CommandResult : std::uint8_t {
  not_a_command,
  handled,
  quit,
};

[[nodiscard]] CommandResult handle_command(const std::string_view message,
                                           scry::Conversation& conversation) {
  if (message == "/quit" || message == "/exit") {
    return CommandResult::quit;
  }
  if (message == "/help") {
    show_help();
    return CommandResult::handled;
  }
  if (message == "/tools") {
    show_tools();
    return CommandResult::handled;
  }
  if (message != "/reset") {
    return CommandResult::not_a_command;
  }

  auto reset = create_conversation();
  if (!reset) {
    std::cerr << "Conversation reset failed: " << reset.error().message << '\n';
    return CommandResult::handled;
  }
  conversation = std::move(*reset);
  std::cout << "Conversation reset.\n";
  return CommandResult::handled;
}

[[nodiscard]] bool run_turn(scry::Harness& harness, scry::Conversation& conversation,
                            ToolActivity& activity, std::string message) {
  auto turn_result = harness.send(conversation, std::move(message));
  if (!turn_result) {
    std::cerr << "Send failed: " << turn_result.error().message << '\n';
    return false;
  }

  auto turn = std::move(*turn_result);
  TurnDisplay display;
  if (!attach_callbacks(turn, display, activity)) {
    return false;
  }
  pump_until_terminal(harness, display);
  return true;
}

[[nodiscard]] int run() {
  const auto base_url = environment_or("SCRY_LOCAL_MODEL_BASE_URL", default_base_url);
  const auto model = environment_or("SCRY_LOCAL_MODEL_MODEL", default_model);
  auto harness_result = create_harness(base_url, model);
  if (!harness_result) {
    std::cerr << "Harness creation failed: " << harness_result.error().message << '\n';
    return 1;
  }
  auto harness = std::move(*harness_result);

  ToolActivity activity;
  if (auto status = register_tools(harness, activity); !status) {
    std::cerr << "Tool registration failed: " << status.error().message << '\n';
    return 1;
  }
  auto conversation_result = create_conversation();
  if (!conversation_result) {
    std::cerr << "Conversation creation failed: " << conversation_result.error().message
              << '\n';
    return 1;
  }
  auto conversation = std::move(*conversation_result);

  std::cout << "Scry tool chat\n"
            << "Model: " << model << '\n'
            << "Endpoint: " << base_url << '\n';
  show_tools();
  std::cout << "Try: What is the current time? Then multiply 17 by 23.\n";
  show_help();

  std::string message;
  while (std::cout << "\nyou> " && std::getline(std::cin, message)) {
    if (message.empty()) {
      continue;
    }
    const auto command = handle_command(message, conversation);
    if (command == CommandResult::quit) {
      break;
    }
    if (command == CommandResult::handled) {
      continue;
    }
    static_cast<void>(run_turn(harness, conversation, activity, std::move(message)));
  }
  std::cout << "\nGoodbye.\n";
  return 0;
}

} // namespace scry_tool_chat

int main() { return scry_tool_chat::run(); }
