// A toolbox: an object whose annotated member functions are tools. Shows described
// and constrained arguments, and a tool that refuses a call.

#include <cstdint>
#include <iostream>
#include <memory>
#include <scry/scry.hpp>
#include <string_view>
#include <utility>

struct WaterArguments {
  [[
    = scrydantic::description{"Bed number"}, = scrydantic::range{.min = 1, .max = 3}
  ]] std::int32_t bed;
  [[= scrydantic::description{"Liters to pour"}]] double liters{1.0};
};

class Greenhouse {
public:
  [[= scrydantic::tool{"Read the temperature in Celsius"}]] double temperature() const {
    return vents_open_ ? 21.0 : 27.0;
  }

  [[= scrydantic::tool{"Open or close the roof vents"}]] void
      set_vents(const bool open) {
    vents_open_ = open;
  }

  [[= scrydantic::tool{"Water one bed"}]] scrydantic::Status
      water(const WaterArguments arguments) {
    if (arguments.liters > tank_) {
      // The model reads this text and can call again.
      return std::unexpected(scrydantic::tool_error("only 5 liters are left"));
    }
    tank_ -= arguments.liters;
    return {};
  }

  double tank() const { return tank_; } // not annotated, so not a tool

private:
  bool vents_open_{false};
  double tank_{5.0};
};

int main(const int argc, char* argv[]) {
  // The registry shares ownership. add(Greenhouse{}) would take it instead.
  const auto house = std::make_shared<Greenhouse>();
  scry::ToolRegistry tools;
  if (auto added = tools.add(house); !added) {
    std::cerr << added.error().message << '\n';
    return 1;
  }

  // Prints the generated schemas. Needs no model.
  if (argc == 2 && std::string_view{argv[1]} == "--schemas") {
    const auto manifest = tools.to_json();
    std::cout << (manifest ? manifest->text : manifest.error().message) << '\n';
    return manifest ? 0 : 1;
  }

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

  auto reply = harness->send_and_wait(
      *conversation, "Open the vents, then pour 2 liters of water on bed 3.");
  std::cout << (reply ? reply->text : reply.error().message) << '\n';
  std::cout << "left in the tank: " << house->tank() << '\n';
}
