#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <utility>

// Any annotated function can be a tool. Reflection generates its JSON schema,
// argument decoder, and result encoder from the signature.
[[= scrydantic::tool{"Get the current weather in a city"}]] std::string
    weather(std::string city) {
  return city + ": 18C and sunny";
}

int main() {
  scry::ToolRegistry tools;
  if (auto added = tools.add<^^weather>(); !added) {
    std::cerr << added.error().message << '\n';
    return 1;
  }

  // A local Ollama server: `ollama pull qwen2.5:7b`.
  auto harness =
      scry::Harness::create({.base_url = "http://127.0.0.1:11434/v1",
                             .model = "qwen2.5:7b",
                             .dialect = scry::ProviderDialect::openai_compatible},
                            std::move(tools));
  auto conversation = scry::Conversation::create();
  if (!harness || !conversation) {
    std::cerr << (harness ? conversation.error() : harness.error()).message << '\n';
    return 1;
  }

  auto reply =
      harness->send_and_wait(*conversation, "Do I need an umbrella in Oslo today?");
  std::cout << (reply ? reply->text : reply.error().message) << '\n';
}
