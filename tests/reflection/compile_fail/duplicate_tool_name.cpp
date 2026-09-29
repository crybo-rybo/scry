#include <scry/tool_registry.hpp>

// Two tools of one toolbox that end with one name. The registry would reject
// the second at runtime too, but the declaration is already wrong.
struct Lamp {
  [[= scry::reflection::tool{"Switch the lamp on"}]] bool toggle() { return true; }
  [[
    = scry::reflection::tool{"Switch the lamp off"},
    = scry::reflection::name{"toggle"}
  ]] bool
  switch_off() {
    return false;
  }
};

void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add(Lamp{});
  (void)status;
}
