#include <scry/tool_registry.hpp>
#include <string>

namespace lamp_tools {
// Decoded arguments are moved into the call, which a non-const lvalue reference
// cannot bind.
[[= scry::reflection::tool{"Rename the lamp"}]] inline bool rename(std::string& name) {
  return !name.empty();
}
} // namespace lamp_tools

void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add<^^lamp_tools::rename>();
  (void)status;
}
