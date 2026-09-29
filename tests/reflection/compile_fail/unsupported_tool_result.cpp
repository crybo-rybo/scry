#include <memory>
#include <scry/tool_registry.hpp>

// Raw JSON has no schema, so a reflected tool cannot return it; the diagnostic
// names the member function inside the toolbox.
struct Lamp {
  [[= scry::reflection::tool{"Describe the lamp"}]] scry::Json describe() const {
    return {.text = "{}"};
  }
};

void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add(std::make_shared<Lamp>());
  (void)status;
}
