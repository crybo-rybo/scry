// Streams an answer from a frame loop. send() returns at once, and everything of
// yours (tools and callbacks) runs inside update(), on this thread.

#include <chrono>
#include <iostream>
#include <memory>
#include <scry/scry.hpp>
#include <string_view>
#include <thread>
#include <utility>

class Game {
public:
  [[= scrydantic::tool{"Read the player's score"}]] int score() const { return score_; }

  void tick() { ++score_; }

private:
  int score_{0};
};

int main() {
  // Declared before the harness, because the registered tool uses it.
  const auto game = std::make_shared<Game>();
  scry::ToolRegistry tools;
  if (auto added = tools.add(game); !added) {
    std::cerr << added.error().message << '\n';
    return 1;
  }

  auto harness =
      scry::Harness::create({.base_url = "http://127.0.0.1:11434/v1",
                             .model = "qwen2.5:7b",
                             .dialect = scry::ProviderDialect::openai_compatible},
                            std::move(tools));
  auto conversation = scry::Conversation::create({.system_prompt = "Be brief."});
  if (!harness || !conversation) {
    std::cerr << (harness ? conversation.error() : harness.error()).message << '\n';
    return 1;
  }

  auto turn = harness->send(
      *conversation, "How is the player doing?",
      {
          .on_text_delta =
              [](const std::string_view text) { std::cout << text << std::flush; },
          .on_tool_call =
              [](const scry::ToolCall& call) {
                std::cout << '[' << call.name << " -> " << call.result.text << "]\n";
              },
          .on_finished =
              [](const scry::Result<scry::Completion>& done) {
                std::cout << (done ? "" : done.error().message) << '\n';
              },
      });
  if (!turn) {
    std::cerr << turn.error().message << '\n';
    return 1;
  }

  while (!turn->finished()) {
    game->tick();
    harness->update({.time_budget = std::chrono::milliseconds{2}});
    std::this_thread::sleep_for(std::chrono::milliseconds{16}); // the rest of the frame
  }
}
