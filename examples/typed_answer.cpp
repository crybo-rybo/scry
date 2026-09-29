// Typed answers: the turn ends with a value of a C++ type instead of free text.
//
// ask<T>() is the blocking form, for command-line programs. send<T>() is the
// poll-friendly form for a host that owns its main loop: the answer arrives in
// Completion::structured inside update(), and the host decodes it with the same
// reflected codec that generated the schema the model was given.

#include <chrono>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

enum class Confidence { low, medium, high };

/// The answer the model must give. Its schema, its strict decoder, and the
/// errors the model sees when it gets the shape wrong are generated from this.
struct Verdict {
  [[= scry::reflection::description{
      "Is the claim supported by evidence?"}]] bool supported{};
  [[= scry::reflection::description{
      "One sentence explaining the verdict"}]] std::string reason{};
  Confidence confidence{Confidence::medium};
};

/// A multi-field answer the model assembles after calling a tool.
struct Itinerary {
  [[= scry::reflection::description{
      "Cities in visiting order"}]] std::vector<std::string>
      stops{};
  [[= scry::reflection::description{"Total days, including travel"}]] int days{};
};

struct DistanceArguments {
  std::string from{};
  std::string to{};
};

[[nodiscard]] std::string_view confidence_name(const Confidence confidence) {
  switch (confidence) {
  case Confidence::low:
    return "low";
  case Confidence::medium:
    return "medium";
  case Confidence::high:
    return "high";
  }
  return "unknown";
}

// Blocking: one question, one decoded answer.
[[nodiscard]] int ask_for_a_verdict(scry::Harness& harness) {
  auto conversation = scry::Conversation::create();
  if (!conversation) {
    std::cerr << conversation.error().message << '\n';
    return 1;
  }
  auto verdict = harness.ask<Verdict>(*conversation, "Is the moon made of cheese?");
  if (!verdict) {
    std::cerr << verdict.error().message << '\n';
    return 1;
  }
  std::cout << (verdict->value.supported ? "supported" : "not supported") << " ("
            << confidence_name(verdict->value.confidence)
            << "): " << verdict->value.reason << '\n';
  if (!verdict->completion.text.empty()) {
    std::cout << "the model also said: " << verdict->completion.text << '\n';
  }
  return 0;
}

// Poll-friendly: the host keeps its own loop, tools run inside update(), and the
// answer arrives with on_finished.
[[nodiscard]] int plan_in_the_main_loop(scry::Harness& harness) {
  auto conversation = scry::Conversation::create(
      {.system_prompt = "Plan trips. Check distances with the tool before answering."});
  if (!conversation) {
    std::cerr << conversation.error().message << '\n';
    return 1;
  }
  bool finished = false;
  int status = 1;
  auto turn = harness.send<Itinerary>(
      *conversation, "Plan three days from Lyon to Milan.",
      {
          .on_text_delta = [](std::string_view text) { std::cout << text; },
          .on_finished =
              [&finished, &status](scry::Result<scry::Completion> completion) {
                finished = true;
                if (!completion) {
                  std::cerr << '\n' << completion.error().message << '\n';
                  return;
                }
                // The answer was validated before the turn completed, so this
                // decode only turns canonical JSON back into the value.
                auto plan =
                    scry::reflection::decode<Itinerary>(*completion->structured);
                if (!plan) {
                  std::cerr << '\n' << plan.error().message << '\n';
                  return;
                }
                std::cout << '\n' << plan->days << " days:";
                for (const auto& stop : plan->stops) {
                  std::cout << ' ' << stop;
                }
                std::cout << " (" << completion->answer_attempt_count
                          << " answer attempt(s))\n";
                status = 0;
              },
      });
  if (!turn) {
    std::cerr << turn.error().message << '\n';
    return 1;
  }
  while (!finished) {
    // A game or UI would render a frame here.
    static_cast<void>(harness.update({.time_budget = std::chrono::milliseconds{2}}));
  }
  return status;
}

} // namespace

int main() {
  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  // Any server that honors a required tool choice works the same way.
  scry::ToolRegistry tools;
  auto registered = tools.add<DistanceArguments>(
      {.name = "distance_km", .description = "Road distance between two cities"},
      [](const DistanceArguments& arguments) {
        return arguments.from == arguments.to ? 0 : 440;
      });
  if (!registered) {
    std::cerr << registered.error().message << '\n';
    return 1;
  }
  auto harness = scry::Harness::create(
      {
          .base_url = "http://127.0.0.1:11434/v1",
          .model = "qwen3:8b",
          .dialect = scry::ProviderDialect::openai_compatible,
      },
      std::move(tools));
  if (!harness) {
    std::cerr << harness.error().message << '\n';
    return 1;
  }
  if (const auto status = ask_for_a_verdict(*harness); status != 0) {
    return status;
  }
  return plan_in_the_main_loop(*harness);
}
