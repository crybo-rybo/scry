#include <memory>
#include <scry/tool_registry.hpp>

// A raw pointer has no JSON form, so a reflected tool cannot return it; the
// diagnostic names the member function inside the toolbox.
struct Lamp {
  [[= scrydantic::tool{"Describe the lamp"}]] const char* describe() const {
    return "lit";
  }
};

void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add(std::make_shared<Lamp>());
  (void)status;
}
