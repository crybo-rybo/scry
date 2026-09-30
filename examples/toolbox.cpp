#include <cstdint>
#include <iostream>
#include <memory>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <utility>

// Every way to declare a reflected tool, in one greenhouse. The schema the model
// sees, the strict decoding of its arguments, and the encoding of each result are
// generated from the declarations; nothing here writes JSON by hand.
namespace greenhouse {

enum class Crop : std::uint8_t {
  tomato,
  lettuce,
  basil,
};

struct Climate {
  double temperature_c{};
  bool vents_open{};
};

// An argument aggregate: the form to reach for when parameters need descriptions,
// defaults, or optional members.
struct WaterArguments {
  [[= scry::reflection::description{"Bed number, from 1 to 3"}]] std::int32_t bed;
  [[= scry::reflection::description{"Litres to pour"}]] double litres{1.0};
};

// A toolbox. Each annotated member function becomes a tool bound to one object,
// and the tool names are the function names. Handlers run on the host thread
// inside Harness::update(), so the state needs no locking.
class Greenhouse {
public:
  // No parameters: the tool takes {}.
  [[= scry::reflection::tool{
      "Read the temperature and whether the roof vents are open"}]] Climate
  climate() const {
    return climate_;
  }

  // Plain parameters: Scry synthesizes the argument object {"open": boolean}.
  [[= scry::reflection::tool{"Open or close the roof vents"}]] Climate
  set_vents(const bool open) {
    climate_.vents_open = open;
    climate_.temperature_c = open ? 21.0 : 27.0;
    return climate_;
  }

  // One aggregate parameter is the argument object, member descriptions included.
  // A Status result reaches the model as {} or as the refusal's text.
  [[= scry::reflection::tool{"Water one bed"}]] scry::Status
  water(const WaterArguments arguments) {
    if (arguments.bed < 1 || arguments.bed > 3) {
      return std::unexpected(scry::tool_error("there are only beds 1 to 3"));
    }
    litres_poured_ += arguments.litres;
    return {};
  }

  // An explicit object parameter is the toolbox itself, not an argument, so this
  // tool takes {}.
  [[= scry::reflection::tool{"Litres poured since the greenhouse opened"}]] double
  litres_so_far(this const Greenhouse& self) {
    return self.litres_poured_;
  }

  // Not a tool: only the host calls it.
  [[nodiscard]] double litres_poured() const noexcept { return litres_poured_; }

private:
  Climate climate_{.temperature_c = 27.0};
  double litres_poured_{};
};

} // namespace greenhouse

// A namespace of free tools, registered together with add<^^almanac>().
namespace almanac {

[[= scry::reflection::tool{"Days until the first expected frost"}]] inline std::int32_t
days_to_frost() {
  return 41;
}

[[= scry::reflection::tool{"Planting advice for one crop"}]] inline std::string
advice(const greenhouse::Crop crop) {
  switch (crop) {
  case greenhouse::Crop::tomato:
    return "Stake them and water at the base.";
  case greenhouse::Crop::lettuce:
    return "Sow every two weeks for a steady harvest.";
  case greenhouse::Crop::basil:
    return "Pinch off flower buds to keep the leaves coming.";
  }
  return {};
}

} // namespace almanac

// One free function, registered on its own with add<^^to_fahrenheit>(). A name
// annotation replaces the identifier as the tool name.
[[
  = scry::reflection::tool{"Convert a Celsius temperature to Fahrenheit"},
  = scry::reflection::name{"celsius_to_fahrenheit"}
]] inline double
to_fahrenheit(const double celsius) {
  return celsius * 9.0 / 5.0 + 32.0;
}

namespace {

// Each call registers all of its tools or none of them.
[[nodiscard]] scry::Status
register_tools(scry::ToolRegistry& tools,
               const std::shared_ptr<greenhouse::Greenhouse>& house) {
  if (auto status = tools.add(house); !status) {
    return status;
  }
  if (auto status = tools.add<^^almanac>(); !status) {
    return status;
  }
  return tools.add<^^to_fahrenheit>();
}

[[nodiscard]] int run_turn(scry::ToolRegistry tools) {
  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
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
  auto conversation = scry::Conversation::create(
      {.system_prompt = "You look after a greenhouse. Use the tools."});
  if (!conversation) {
    std::cerr << conversation.error().message << '\n';
    return 1;
  }
  const auto answer = harness->send_and_wait(
      *conversation, "It feels hot in here. Cool it down, water bed 2, and tell me "
                     "the temperature in Fahrenheit.");
  if (!answer) {
    std::cerr << answer.error().message << '\n';
    return 1;
  }
  std::cout << answer->text << '\n';
  return 0;
}

} // namespace

int main(const int argc, char* argv[]) {
  // The host keeps a handle to the toolbox, so it can read what the tools did.
  // Passing a greenhouse::Greenhouse by value instead would move it into the
  // registry, which would then own the only copy.
  const auto house = std::make_shared<greenhouse::Greenhouse>();

  scry::ToolRegistry tools;
  if (const auto registered = register_tools(tools, house); !registered) {
    std::cerr << registered.error().message << '\n';
    return 1;
  }

  // --tool-manifest prints every generated schema without contacting a model.
  if (argc == 2 && std::string_view{argv[1]} == "--tool-manifest") {
    const auto manifest = tools.to_json();
    if (!manifest) {
      std::cerr << manifest.error().message << '\n';
      return 1;
    }
    std::cout << manifest->text << '\n';
    return 0;
  }

  const int status = run_turn(std::move(tools));
  std::cout << "litres poured: " << house->litres_poured() << '\n';
  return status;
}
