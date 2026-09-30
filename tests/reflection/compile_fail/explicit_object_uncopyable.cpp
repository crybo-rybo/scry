#include <scry/tool_registry.hpp>

struct Lamp {
  Lamp() = default;
  Lamp(const Lamp&) = delete;
  Lamp(Lamp&&) = default;
  Lamp& operator=(const Lamp&) = delete;
  Lamp& operator=(Lamp&&) = default;
  ~Lamp() = default;

  bool lit{};
  [[= scry::reflection::tool{"Report the lamp"}]] bool read(this Lamp self) {
    return self.lit;
  }
};

// A by-value explicit object parameter copies the toolbox the registry holds.
void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add(Lamp{});
  (void)status;
}
