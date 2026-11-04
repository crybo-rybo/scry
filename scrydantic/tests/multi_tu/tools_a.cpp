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

std::vector<scrydantic::BoundTool<>> bind_inventory_from_a() {
  return scrydantic::bind<^^inventory_tools>();
}

std::vector<scrydantic::BoundTool<>> bind_echo_from_a() {
  return scrydantic::bind<^^echo_tools::echo>();
}
