#include <scry/tool_registry.hpp>

// Two tools of one toolbox that end with one name. The registry would reject
// the second at runtime too, but the declaration is already wrong.
struct Lamp {
  [[= scrydantic::tool{"Switch the lamp on"}]] bool toggle() { return true; }
  [[ = scrydantic::tool{"Switch the lamp off"},
     = scrydantic::name{"toggle"} ]] bool switch_off() {
    return false;
  }
};

void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add(Lamp{});
  (void)status;
}
