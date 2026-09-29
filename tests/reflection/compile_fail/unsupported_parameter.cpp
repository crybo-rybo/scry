#include <cstdint>
#include <scry/tool_registry.hpp>

namespace lamp_tools {
[[= scry::reflection::tool{"Label the lamp"}]] inline bool label(std::int32_t slot,
                                                                 char initial) {
  return slot > 0 && initial != '\0';
}
} // namespace lamp_tools

void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add<^^lamp_tools::label>();
  (void)status;
}
