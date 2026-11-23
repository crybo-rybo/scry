// ask<T>() makes the model answer with a C++ struct.

#include <iostream>
#include <scry/scry.hpp>
#include <string>

struct Verdict {
  [[= scrydantic::description{"Is the claim true?"}]] bool supported{};
  [[= scrydantic::description{"One sentence explaining why"}]] std::string reason{};
};

int main() {
  auto harness =
      scry::Harness::create({.base_url = "http://127.0.0.1:11434/v1",
                             .model = "qwen2.5:7b",
                             .dialect = scry::ProviderDialect::openai_compatible});
  auto conversation = scry::Conversation::create();
  if (!harness || !conversation) {
    std::cerr << (harness ? conversation.error() : harness.error()).message << '\n';
    return 1;
  }

  auto answer = harness->ask<Verdict>(*conversation, "Is the moon made of cheese?");
  if (!answer) {
    std::cerr << answer.error().message << '\n';
    return 1;
  }
  std::cout << std::boolalpha << answer->value.supported << ": " << answer->value.reason
            << '\n';
}
