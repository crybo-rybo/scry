#include <scry/tool_registry.hpp>

namespace lamp_tools {
inline bool switch_on() { return true; }
} // namespace lamp_tools

void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add<^^lamp_tools>();
  (void)status;
}
