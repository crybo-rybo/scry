// One question with one reflected tool, answered by a local llamad daemon over
// its Unix socket. Built only when Scry is configured with SCRY_WITH_LLAMAD=ON.
//
//   llamad --model models/qwen2.5-0.5b-instruct-q4_k_m.gguf --socket /tmp/llamad.sock
//   ./scry_llamad_chat_example /tmp/llamad.sock

#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <utility>

namespace {

struct ClockArgs {
  [[= scry::reflection::description{
      "IANA time zone, such as Europe/Oslo"}]] std::string zone;
};

struct ClockReading {
  std::string zone;
  std::string time;
};

} // namespace

int main(const int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <llamad socket path>\n";
    return 2;
  }

  // The daemon serves the one model it loaded, so `model` only has to be
  // non-empty. Temperature 0 is greedy decoding on llamad.
  auto harness_result = scry::Harness::create({
      .base_url = "unix:" + std::string{argv[1]},
      .model = "local",
      .dialect = scry::ProviderDialect::llamad,
      .sampling = {.temperature = 0.0, .max_tokens = 256},
  });
  if (!harness_result) {
    std::cerr << harness_result.error().message << '\n';
    return 1;
  }
  auto harness = std::move(*harness_result);

  const auto registered = scry::reflection::add<ClockArgs>(
      harness.tools(),
      {.name = "clock", .description = "Read the current time in one time zone"},
      [](ClockArgs args) {
        return ClockReading{.zone = std::move(args.zone), .time = "12:00"};
      });
  if (!registered) {
    std::cerr << registered.error().message << '\n';
    return 1;
  }

  auto conversation = scry::Conversation::create();
  if (!conversation) {
    std::cerr << conversation.error().message << '\n';
    return 1;
  }
  const auto completion =
      harness.send_and_wait(*conversation, "What time is it in Oslo? Use the clock.");
  if (!completion) {
    std::cerr << completion.error().message << '\n';
    return 1;
  }
  std::cout << completion->text << '\n'
            << "tool calls: " << completion->tool_call_count << '\n';
  return 0;
}
