#include <memory>
#include <scry/tool_registry.hpp>

struct Lamp {
  bool lit{};
  [[= scry::reflection::tool{"Toggle the lamp"}]] bool toggle(this Lamp& self) {
    return self.lit = !self.lit;
  }
};

// A const toolbox cannot bind a non-const reference, exactly as it cannot call a
// non-const member function.
void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add(std::make_shared<const Lamp>());
  (void)status;
}
