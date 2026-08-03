#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <format>
#include <iostream>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace scry_tool_chat {

constexpr std::string_view default_base_url = "http://127.0.0.1:11434/v1";
constexpr std::string_view default_model = "qwen3:8b";

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

void print_tool_outcome(const std::string_view name, const std::string_view arguments,
                        const scry::Result<scry::Json>& result) {
  std::cout << "tool> " << name << ' ' << arguments << '\n';
  if (result) {
    std::cout << "result> " << result->text << '\n';
  } else {
    std::cout << "error> " << result.error().message << '\n';
  }
}

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

[[nodiscard]] std::string_view trim_ascii_whitespace(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                           text.front() == '\n' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                           text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

[[nodiscard]] std::optional<double>
parse_json_number_field(const std::string_view json, const std::string_view key) {
  const auto needle = std::format("\"{}\"", key);
  const auto key_pos = json.find(needle);
  if (key_pos == std::string_view::npos) {
    return std::nullopt;
  }

  auto cursor = json.substr(key_pos + needle.size());
  cursor = trim_ascii_whitespace(cursor);
  if (cursor.empty() || cursor.front() != ':') {
    return std::nullopt;
  }
  cursor.remove_prefix(1);
  cursor = trim_ascii_whitespace(cursor);
  if (cursor.empty()) {
    return std::nullopt;
  }

  // libc++ (Clang/AppleClang) does not provide floating-point std::from_chars.
  // Copy to a null-terminated buffer so strtod can parse portably.
  const std::string token{cursor};
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(token.c_str(), &end);
  if (end == token.c_str() || errno == ERANGE || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

using ArithmeticOperation = double (*)(double, double);

[[nodiscard]] scry::Result<scry::Json> calculate(const scry::Json& arguments,
                                                 const ArithmeticOperation operation) {
  const auto a = parse_json_number_field(arguments.text, "a");
  const auto b = parse_json_number_field(arguments.text, "b");
  if (!a || !b) {
    return std::unexpected(
        tool_error("arithmetic tools require finite numeric a and b fields"));
  }

  const double value = operation(*a, *b);
  if (!std::isfinite(value)) {
    return std::unexpected(tool_error("arithmetic result is not finite"));
  }
  return scry::Json{.text = std::format(R"({{"result":{}}})", value)};
}

[[nodiscard]] double add(const double a, const double b) noexcept { return a + b; }

[[nodiscard]] double multiply(const double a, const double b) noexcept { return a * b; }

[[nodiscard]] scry::Status register_datetime_tool(scry::Harness& harness) {
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
      [](scry::Json arguments) -> scry::Result<scry::Json> {
        auto result = current_datetime(arguments);
        print_tool_outcome("get_current_datetime", arguments.text, result);
        return result;
      });
}

[[nodiscard]] scry::Status
register_arithmetic_tool(scry::Harness& harness, const std::string_view name,
                         const std::string_view description,
                         const ArithmeticOperation operation) {
  std::string tool_name{name};
  return harness.tools().add(
      scry::ToolDefinition{
          .name = tool_name,
          .description = std::string{description},
          .input_schema =
              {
                  .text =
                      R"({"type":"object","properties":{"a":{"type":"number","description":"First operand"},"b":{"type":"number","description":"Second operand"}},"required":["a","b"],"additionalProperties":false})",
              },
      },
      [tool_name = std::move(tool_name),
       operation](scry::Json arguments) -> scry::Result<scry::Json> {
        auto result = calculate(arguments, operation);
        print_tool_outcome(tool_name, arguments.text, result);
        return result;
      });
}

[[nodiscard]] scry::Status register_tools(scry::Harness& harness) {
  if (auto status = register_datetime_tool(harness); !status) {
    return status;
  }
  if (auto status = register_arithmetic_tool(
          harness, "add",
          "Add two numbers. Use this tool for addition instead of mental arithmetic.",
          add);
      !status) {
    return status;
  }
  return register_arithmetic_tool(
      harness, "multiply",
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

[[nodiscard]] bool attach_callbacks(scry::Turn& turn, TurnDisplay& display) {
  auto status = turn.on_text_delta(
      [&display](const std::string_view delta) { display.show_delta(delta); });
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
                            std::string message) {
  auto turn_result = harness.send(conversation, std::move(message));
  if (!turn_result) {
    std::cerr << "Send failed: " << turn_result.error().message << '\n';
    return false;
  }

  auto turn = std::move(*turn_result);
  TurnDisplay display;
  if (!attach_callbacks(turn, display)) {
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

  if (auto status = register_tools(harness); !status) {
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
    static_cast<void>(run_turn(harness, conversation, std::move(message)));
  }
  std::cout << "\nGoodbye.\n";
  return 0;
}

} // namespace scry_tool_chat

int main() { return scry_tool_chat::run(); }
