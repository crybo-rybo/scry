#include <scry/tool_registry.hpp>

struct Lamp {
  bool lit{};
  [[= scry::reflection::tool{"Toggle the lamp"}]] bool toggle() { return lit = !lit; }
};

// A non-static member function needs an object, which only a toolbox supplies.
void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add<^^Lamp::toggle>();
  (void)status;
}
