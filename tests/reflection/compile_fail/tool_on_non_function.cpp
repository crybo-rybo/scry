#include <scry/tool_registry.hpp>

namespace lamp_tools {
[[= scry::reflection::tool{"Switch the lamp on"}]] inline bool switch_on() {
  return true;
}

// The annotation applies to functions; a variable carrying it is a mistake the
// namespace walk reports rather than skips.
[[= scry::reflection::tool{"Brightness"}]] inline int brightness = 3;
} // namespace lamp_tools

void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add<^^lamp_tools>();
  (void)status;
}
