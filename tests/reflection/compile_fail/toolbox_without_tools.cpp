#include <memory>
#include <scry/tool_registry.hpp>

// A class with member functions, none of them annotated as a tool.
class Lamp {
public:
  void toggle() { lit_ = !lit_; }

private:
  bool lit_{};
};

void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add(std::make_shared<Lamp>());
  (void)status;
}
