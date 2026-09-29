#include "inventory.hpp"

#include <string>

namespace inventory_tools {

[[= scry::reflection::tool{
    "Only translation unit A declares this tool"}]] inline std::string
only_a() {
  return "a";
}

} // namespace inventory_tools

scry::Status register_from_a(scry::ToolRegistry& registry) {
  return registry.add<^^inventory_tools>();
}
