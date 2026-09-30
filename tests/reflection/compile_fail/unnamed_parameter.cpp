#include <cstdint>
#include <scry/tool_registry.hpp>

namespace lamp_tools {
// A synthesized argument is named after its parameter, so every one needs a name.
[[= scry::reflection::tool{"Dim the lamp"}]] inline bool dim(std::int32_t level,
                                                             std::int32_t) {
  return level > 0;
}
} // namespace lamp_tools

void register_tools(scry::ToolRegistry& registry) {
  const auto status = registry.add<^^lamp_tools::dim>();
  (void)status;
}
