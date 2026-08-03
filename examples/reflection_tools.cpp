#include <iostream>
#include <scry/config.hpp>
#include <scry/harness.hpp>
#include <scry/reflection.hpp>
#include <string>
#include <utility>

namespace {

struct StatusArguments {
  [[= scry::reflection::description{
      "Include a human-readable state label in the result"}]] bool verbose{false};
};

struct StatusResult {
  bool running{};
  std::string state{};
};

} // namespace

int main() {
  auto created = scry::Harness::create(scry::Config{
      .base_url = "https://api.anthropic.com",
      .api_key = "load-from-the-host-secret-store",
      .model = "configured-by-the-host",
  });
  if (!created) {
    std::cerr << created.error().message << '\n';
    return 1;
  }
  auto harness = std::move(*created);

  auto registration = scry::reflection::add<StatusArguments>(
      harness.tools(),
      {
          .name = "get_application_status",
          .description = "Report the state of the host application's main loop",
      },
      [](StatusArguments arguments) {
        return StatusResult{
            .running = true,
            .state = arguments.verbose ? "running" : "",
        };
      });
  if (!registration) {
    std::cerr << registration.error().message << '\n';
    return 1;
  }

  std::cout << scry::reflection::input_schema_v<StatusArguments> << '\n';
  return 0;
}
