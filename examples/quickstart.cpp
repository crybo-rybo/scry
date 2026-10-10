#include <chrono>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <thread>
#include <utility>

// Each annotated member function becomes a tool that is bound to this object. Scry
// generates the JSON schema from the signature and decodes the model's arguments.
class HostTools {
public:
  [[= scry::reflection::tool{"Report the state of one subsystem"}]] std::string
      subsystem_state(std::string name) const {
    return name + " is running";
  }
};

int fail(const scry::Error& error) {
  std::cerr << error.message << '\n';
  return 1;
}

int main() {
  scry::ToolRegistry tools;
  if (auto added = tools.add(HostTools{}); !added)
    return fail(added.error());

  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  auto harness =
      scry::Harness::create({.base_url = "http://127.0.0.1:11434/v1",
                             .model = "qwen3:8b",
                             .dialect = scry::ProviderDialect::openai_compatible},
                            std::move(tools));
  if (!harness)
    return fail(harness.error());

  auto conversation = scry::Conversation::create();
  if (!conversation)
    return fail(conversation.error());

  const auto print = [](scry::Result<scry::Completion> done) {
    if (done)
      std::cout << done->text << '\n';
    else
      fail(done.error());
  };
  auto turn =
      harness->send(*conversation, "Is the renderer running?", {.on_finished = print});
  if (!turn)
    return fail(turn.error());

  while (!turn->finished()) {
    harness->update(); // tool handlers and callbacks run here, on this thread
    std::this_thread::sleep_for(std::chrono::milliseconds{1}); // your frame
  }
}
