#include "world.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

class Application final {
public:
  [[nodiscard]] bool running() const noexcept { return running_; }

  void show_delta(const std::string_view delta) {
    streamed_ = true;
    std::cout << delta << std::flush;
  }

  void show_completion(const scry::Completion& completion) {
    if (!streamed_) {
      std::cout << completion.text;
    }
    std::cout << '\n';
    running_ = false;
  }

  void show_error(const std::string_view message) {
    std::cerr << message << '\n';
    exit_code_ = 1;
    running_ = false;
  }

  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

private:
  bool running_{true};
  bool streamed_{false};
  int exit_code_{};
};

[[nodiscard]] std::string environment(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

[[nodiscard]] std::string prompt_from_arguments(const int argc, char** argv) {
  if (argc < 2) {
    return "Use look, move north once, move east once, then report the final "
           "position.";
  }

  std::string prompt{argv[1]};
  for (int index = 2; index < argc; ++index) {
    prompt.push_back(' ');
    prompt.append(argv[index]);
  }
  return prompt;
}

[[nodiscard]] scry::Result<scry::Harness> create_harness(std::string base_url,
                                                         std::string model) {
  return scry::Harness::create({
      .base_url = std::move(base_url),
      .api_key = environment("SCRY_LOCAL_MODEL_API_KEY"),
      .model = std::move(model),
      .dialect = scry::ProviderDialect::openai_compatible,
      .sampling = {.temperature = 0.0},
  });
}

[[nodiscard]] bool add_callbacks(scry::Turn& turn, Application& app) {
  auto status = turn.on_text_delta(
      [&app](const std::string_view delta) { app.show_delta(delta); });
  if (!status) {
    app.show_error(status.error().message);
    return false;
  }
  status = turn.on_complete(
      [&app](const scry::Completion& completion) { app.show_completion(completion); });
  if (!status) {
    app.show_error(status.error().message);
    return false;
  }
  status = turn.on_error(
      [&app](const scry::Error& error) { app.show_error(error.message); });
  if (!status) {
    app.show_error(status.error().message);
    return false;
  }
  status = turn.on_cancelled(
      [&app](const scry::Cancelled&) { app.show_error("Turn cancelled"); });
  if (!status) {
    app.show_error(status.error().message);
    return false;
  }
  return true;
}

void pump_until_terminal(scry::Harness& harness, const Application& app) {
  while (app.running()) {
    const auto stats = harness.update({
        .time_budget = std::chrono::milliseconds{2},
        .max_callbacks = 32,
    });
    if (stats.callbacks_delivered == 0 && stats.events_remaining == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }
}

[[nodiscard]] int run(const int argc, char** argv) {
  auto base_url = environment("SCRY_LOCAL_MODEL_BASE_URL");
  auto model = environment("SCRY_LOCAL_MODEL_MODEL");
  if (base_url.empty() || model.empty()) {
    std::cerr << "Set SCRY_LOCAL_MODEL_BASE_URL and SCRY_LOCAL_MODEL_MODEL. "
                 "SCRY_LOCAL_MODEL_API_KEY is optional.\n";
    return 2;
  }

  auto harness_result = create_harness(std::move(base_url), std::move(model));
  if (!harness_result) {
    std::cerr << harness_result.error().message << '\n';
    return 1;
  }
  auto harness = std::move(*harness_result);

  auto world = std::make_shared<scry_showcase::npc::World>();
  if (auto registration =
          scry_showcase::npc::register_world_tools(harness.tools(), world);
      !registration) {
    std::cerr << registration.error().message << '\n';
    return 1;
  }

  auto conversation_result = scry::Conversation::create({
      .system_prompt =
          "You control an NPC on a bounded grid. Use look before moving. Use only "
          "the supplied tools to inspect or change the NPC position.",
  });
  if (!conversation_result) {
    std::cerr << conversation_result.error().message << '\n';
    return 1;
  }
  auto conversation = std::move(*conversation_result);

  auto turn_result = harness.send(conversation, prompt_from_arguments(argc, argv));
  if (!turn_result) {
    std::cerr << turn_result.error().message << '\n';
    return 1;
  }
  auto turn = std::move(*turn_result);

  Application app;
  if (!add_callbacks(turn, app)) {
    return app.exit_code();
  }
  pump_until_terminal(harness, app);

  std::cout << "Final world: " << world->look().text << '\n';
  return app.exit_code();
}

} // namespace

int main(const int argc, char** argv) { return run(argc, argv); }
