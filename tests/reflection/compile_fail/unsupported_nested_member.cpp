#include <chrono>
#include <scry/reflection.hpp>
#include <string>

// The diagnostic has to name the member path down to the offending value.
struct Window {
  std::chrono::seconds when{};
};

struct ForecastArgs {
  std::string city;
  Window window{};
};

void register_tool(scry::ToolRegistry& registry) {
  const auto status = registry.add<ForecastArgs>(
      {
          .name = "forecast",
          .description = "Must not compile",
      },
      [](ForecastArgs) { return 0; });
  (void)status;
}
