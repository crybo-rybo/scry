#include "inventory.hpp"

#include <string>

namespace inventory_tools {

[[= scry::reflection::tool{
    "Only translation unit B declares this tool"}]] inline std::string
only_b() {
  return "b";
}

} // namespace inventory_tools

scry::Status register_from_b(scry::ToolRegistry& registry) {
  return registry.add<^^inventory_tools>();
}
