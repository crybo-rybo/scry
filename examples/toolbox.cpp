#include <cstdint>
#include <iostream>
#include <memory>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <utility>

// Every way to declare a reflected tool, in one greenhouse. Schemas, argument
// decoding, and result encoding are generated from the declarations.
enum class Crop : std::uint8_t { tomato, basil };

struct Climate {
  double temperature_c{};
  bool vents_open{};
};

// An argument aggregate: the form for descriptions, defaults, or optional members.
struct WaterArguments {
  [[= scry::reflection::description{"Bed number, from 1 to 3"}]] std::int32_t bed;
  [[= scry::reflection::description{"Litres to pour"}]] double litres{1.0};
};

// A toolbox: each annotated member function is a tool bound to one object. Its
// handlers run on the host thread inside Harness::update(), so no locking.
class Greenhouse {
public:
  // No parameters: the tool takes {}.
  [[= scry::reflection::tool{"Read the temperature and the roof vents"}]] Climate
      climate() const {
    return climate_;
  }

  // Plain parameters: Scry synthesizes the argument object {"open": boolean}.
  [[= scry::reflection::tool{"Open or close the roof vents"}]] Climate
      set_vents(const bool open) {
    climate_ = {.temperature_c = open ? 21.0 : 27.0, .vents_open = open};
    return climate_;
  }

  // One aggregate parameter is the argument object. A Status result reaches the
  // model as {} or as the refusal's text.
  [[= scry::reflection::tool{"Water one bed"}]] scry::Status
      water(const WaterArguments arguments) {
    if (arguments.bed < 1 || arguments.bed > 3) {
      return std::unexpected(scry::tool_error("there are only beds 1 to 3"));
    }
    litres_ += arguments.litres;
    return {};
  }

  [[nodiscard]] double litres() const noexcept { return litres_; } // not a tool

private:
  Climate climate_{.temperature_c = 27.0};
  double litres_{};
};

// A namespace of free tools, registered together with add<^^almanac>().
namespace almanac {
[[= scry::reflection::tool{"Planting advice for one crop"}]] inline std::string
    advice(const Crop crop) {
  return crop == Crop::tomato ? "Stake them." : "Pinch off the flower buds.";
}
} // namespace almanac

// One function, registered with add<^^to_fahrenheit>() and renamed by annotation.
[[
  = scry::reflection::tool{"Convert Celsius to Fahrenheit"},
  = scry::reflection::name{"celsius_to_fahrenheit"}
]] inline double to_fahrenheit(const double celsius) {
  return celsius * 9.0 / 5.0 + 32.0;
}

int main(const int argc, char* argv[]) {
  // A shared_ptr keeps a host handle on the toolbox; add(Greenhouse{}) would move
  // it into the registry instead. Each add() registers all its tools or none.
  const auto house = std::make_shared<Greenhouse>();
  scry::ToolRegistry tools;
  for (const auto& status :
       {tools.add(house), tools.add<^^almanac>(), tools.add<^^to_fahrenheit>()}) {
    if (!status) {
      std::cerr << status.error().message << '\n';
      return 1;
    }
  }

  // --tool-manifest prints every generated schema without contacting a model.
  if (argc == 2 && std::string_view{argv[1]} == "--tool-manifest") {
    const auto manifest = tools.to_json();
    std::cout << (manifest ? manifest->text : manifest.error().message) << '\n';
    return manifest ? 0 : 1;
  }

  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  auto harness =
      scry::Harness::create({.base_url = "http://127.0.0.1:11434/v1",
                             .model = "qwen3:8b",
                             .dialect = scry::ProviderDialect::openai_compatible},
                            std::move(tools));
  auto conversation = scry::Conversation::create();
  if (!harness || !conversation) {
    std::cerr << (harness ? conversation.error() : harness.error()).message << '\n';
    return 1;
  }
  const auto answer = harness->send_and_wait(
      *conversation, "Cool the greenhouse, water bed 2, and give me the temperature "
                     "in Fahrenheit.");
  std::cout << (answer ? answer->text : answer.error().message) << '\n';
  std::cout << "litres poured: " << house->litres() << '\n';
  return answer ? 0 : 1;
}
