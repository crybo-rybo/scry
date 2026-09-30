#include <memory>
#include <scry/tool_registry.hpp>

struct Lamp {
  bool lit{};
  [[= scry::reflection::tool{"Toggle the lamp"}]] bool toggle(this Lamp&& self) {
    return self.lit = !self.lit;
  }
};

// The registry holds the toolbox as an lvalue, which an rvalue reference cannot
// bind, exactly as for a &&-qualified member function.
void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add(std::make_shared<Lamp>());
  (void)status;
}
