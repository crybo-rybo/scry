#include <scry/tool_registry.hpp>

struct Lamp {
  bool lit{};
  [[= scry::reflection::tool{"Toggle the lamp"}]] bool toggle() { return lit = !lit; }
};

// The registry never borrows a toolbox: an lvalue would leave the caller's object
// and the registered one silently different.
void register_tools(scry::ToolRegistry& registry, Lamp& lamp) {
  const auto status = registry.add(lamp);
  (void)status;
}
