// Typed answers: the turn ends with a value of a C++ type instead of free text.
//
// ask<T>() blocks for the answer. send<T>() is the poll-friendly form: the
// answer arrives in Completion::structured inside update(), for
// scry::reflection::decode<T>(). send_structured() and
// send_and_wait_structured() take a hand-written ResponseFormat instead.

#include <expected>
#include <iostream>
#include <scry/scry.hpp>
#include <string>

namespace {

/// The answer the model must give. Its schema, its strict decoder, and the
/// errors the model sees when it gets the shape wrong are generated from this.
struct Verdict {
  [[= scry::reflection::description{
      "Is the claim supported by evidence?"}]] bool supported{};
  [[= scry::reflection::description{
      "One sentence explaining the verdict"}]] std::string reason{};
};

// An answer shape known only at runtime: the schema is hand-written and the
// validator owns its checks; its model_message is what the model reads when it
// rejects an answer, and the model tries again.
[[nodiscard]] scry::ResponseFormat score_format() {
  return {
      .name = "score",
      .schema = {.text = R"({"type":"object","required":["score"],)"
                         R"("properties":{"score":{"type":"integer"}}})"},
      .validate = [](const scry::Json& answer) -> scry::Status {
        auto parsed = scry::JsonView::parse(answer);
        if (!parsed || !parsed->find("score")) {
          return std::unexpected(scry::tool_error("score is required"));
        }
        return {};
      },
  };
}

} // namespace

int main() {
  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  // Any server that honors a required tool choice works the same way.
  auto harness = scry::Harness::create({
      .base_url = "http://127.0.0.1:11434/v1",
      .model = "qwen3:8b",
      .dialect = scry::ProviderDialect::openai_compatible,
  });
  auto conversation = scry::Conversation::create();
  if (!harness || !conversation) {
    std::cerr << (harness ? conversation.error() : harness.error()).message << '\n';
    return 1;
  }

  auto verdict = harness->ask<Verdict>(*conversation, "Is the moon made of cheese?");
  if (!verdict) {
    std::cerr << verdict.error().message << '\n';
    return 1;
  }
  std::cout << (verdict->value.supported ? "supported: " : "not supported: ")
            << verdict->value.reason << '\n';

  auto scored = harness->send_and_wait_structured(
      *conversation, "Rate that claim from 0 to 10.", score_format());
  if (!scored) {
    std::cerr << scored.error().message << '\n';
    return 1;
  }
  std::cout << "score: " << scored->structured->text << '\n';
  return 0;
}
