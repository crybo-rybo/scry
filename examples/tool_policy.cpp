#include <chrono>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <thread>
#include <utility>

namespace {

// A one-room world, so the tool has a domain rule the model can violate.
enum class Direction {
  north,
  south,
  east,
  west,
};

struct MoveArguments {
  [[= scry::reflection::description{
      "Compass direction to step toward"}]] Direction direction{};
};

struct MoveResult {
  std::string room{};
};

// A refusal is not a framework failure: the turn continues and the model gets a
// chance to pick another direction. Only the first string reaches the model; the
// second stays in the host's logs.
[[nodiscard]] scry::Result<MoveResult> move(const MoveArguments arguments) {
  if (arguments.direction != Direction::north) {
    return std::unexpected(scry::tool_error(
        "a wall blocks that direction; only north is open from the entrance hall",
        "player attempted a blocked move from the entrance hall"));
  }
  return MoveResult{.room = "library"};
}

// An unknown tool name and an argument that fails the generated schema arrive
// here the same way a refusal does, so one branch covers every tool outcome.
[[nodiscard]] scry::TurnCallbacks policy_callbacks(bool& finished) {
  return {
      .on_tool_call =
          [](const scry::ToolCall& call) {
            std::cout << (call.is_error ? "refused " : "ran ") << call.name << ": "
                      << call.result.text << '\n';
          },
      .on_finished =
          [&finished](scry::Result<scry::Completion> completed) {
            if (completed) {
              std::cout << completed->text << '\n';
            } else {
              std::cerr << completed.error().message << '\n';
            }
            finished = true;
          },
  };
}

} // namespace

int main() {
  scry::ToolRegistry tools;
  if (const auto registered = scry::reflection::add<MoveArguments>(
          tools, {.name = "move", .description = "Step one room in a direction"}, move);
      !registered) {
    std::cerr << registered.error().message << '\n';
    return 1;
  }

  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  auto harness_result = scry::Harness::create(
      {
          .base_url = "http://127.0.0.1:11434/v1",
          .model = "qwen3:8b",
          .dialect = scry::ProviderDialect::openai_compatible,
      },
      std::move(tools));
  if (!harness_result) {
    std::cerr << harness_result.error().message << '\n';
    return 1;
  }
  auto harness = std::move(*harness_result);

  auto conversation_result = scry::Conversation::create(
      {.system_prompt = "You are in an entrance hall. Use the move tool."});
  if (!conversation_result) {
    std::cerr << conversation_result.error().message << '\n';
    return 1;
  }
  auto conversation = std::move(*conversation_result);

  bool finished = false;
  const auto turn =
      harness.send(conversation, "Walk east.", policy_callbacks(finished));
  if (!turn) {
    std::cerr << turn.error().message << '\n';
    return 1;
  }

  while (!finished) {
    const auto stats = harness.update({.time_budget = std::chrono::milliseconds{2}});
    if (stats.callbacks_delivered == 0 && stats.events_remaining == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }

  return 0;
}
