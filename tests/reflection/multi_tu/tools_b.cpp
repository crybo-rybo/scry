#include "inventory.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace inventory_tools {

[[= scry::reflection::tool{
    "Only translation unit B declares this tool"}]] inline std::string
only_b() {
  return "b";
}

} // namespace inventory_tools

namespace echo_tools {

[[= scry::reflection::tool{"Echo a quantity"}]] std::int32_t
echo(std::int32_t quantity);

} // namespace echo_tools

scry::Status register_inventory_from_b(scry::ToolRegistry& registry) {
  return registry.add<^^inventory_tools>();
}

scry::Status register_from_b(UnitRegistries& registries) {
  if (auto status = register_inventory_from_b(registries.inventory); !status) {
    return status;
  }
  if (auto status = registries.echo.add<^^echo_tools::echo>(); !status) {
    return status;
  }
  if (auto status = registries.shared_toolbox.add(std::make_shared<Tally>()); !status) {
    return status;
  }
  return registries.owned_toolbox.add(Tally{});
}
