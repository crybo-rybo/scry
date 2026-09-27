#include <algorithm>
#include <cstddef>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t trial_count = 3;

// Each trial starts from an empty Conversation, so the prompt, history, model, and
// sampling values are identical every time and only the server's randomness is left.
[[nodiscard]] scry::Result<std::string> run_trial(scry::Harness& harness) {
  auto conversation =
      scry::Conversation::create({.system_prompt = "Answer in one short sentence."});
  if (!conversation) {
    return std::unexpected(std::move(conversation.error()));
  }
  auto completion =
      harness.send_and_wait(*conversation, "Invent a name for a roadside tavern.");
  if (!completion) {
    return std::unexpected(std::move(completion.error()));
  }
  return std::move(completion->text);
}

} // namespace

int main() {
  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  // The temperature keeps sampling random; the seed fixes where that randomness
  // starts. Whether the trials then agree is the server's promise, not Scry's.
  auto harness_result = scry::Harness::create({
      .base_url = "http://127.0.0.1:11434/v1",
      .model = "qwen3:8b",
      .dialect = scry::ProviderDialect::openai_compatible,
      .sampling = {.temperature = 0.8, .seed = 42},
  });
  if (!harness_result) {
    std::cerr << harness_result.error().message << '\n';
    return 1;
  }
  auto harness = std::move(*harness_result);

  std::vector<std::string> answers;
  for (std::size_t trial = 1; trial <= trial_count; ++trial) {
    auto answer = run_trial(harness);
    if (!answer) {
      std::cerr << answer.error().message << '\n';
      return 1;
    }
    std::cout << "trial " << trial << ": " << *answer << '\n';
    answers.push_back(std::move(*answer));
  }

  const bool repeated =
      std::ranges::all_of(answers, [&answers](const std::string& answer) {
        return answer == answers.front();
      });
  std::cout << (repeated ? "every trial matched\n"
                         : "the trials differed; this server does not repeat seeded "
                           "output exactly\n");
  return 0;
}
