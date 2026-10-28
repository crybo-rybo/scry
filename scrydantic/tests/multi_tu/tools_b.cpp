#include "inventory.hpp"

#include <cstdint>
#include <string>

namespace inventory_tools {
[[= scrydantic::tool{"Only unit B declares this tool"}]] inline std::string only_b() {
  return "b";
}
} // namespace inventory_tools

namespace echo_tools {
[[= scrydantic::tool{"Echo a quantity"}]] std::int32_t echo(std::int32_t quantity);
} // namespace echo_tools

std::vector<scrydantic::BoundTool<>> bind_inventory_from_b() {
  return scrydantic::bind<^^inventory_tools>();
}

std::vector<scrydantic::BoundTool<>> bind_echo_from_b() {
  return scrydantic::bind<^^echo_tools::echo>();
}
