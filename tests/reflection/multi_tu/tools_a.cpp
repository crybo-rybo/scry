#include "inventory.hpp"

#include <cstdint>
#include <string>

namespace inventory_tools {
[[= scrydantic::tool{"Only unit A declares this tool"}]] inline std::string only_a() {
  return "a";
}
} // namespace inventory_tools

namespace echo_tools {
[[= scrydantic::tool{"Echo a count"}]] std::int32_t echo(std::int32_t count);
} // namespace echo_tools

scry::Status register_inventory_from_a(scry::ToolRegistry& registry) {
  return registry.add<^^inventory_tools>();
}

scry::Status register_echo_from_a(scry::ToolRegistry& registry) {
  return registry.add<^^echo_tools::echo>();
}
