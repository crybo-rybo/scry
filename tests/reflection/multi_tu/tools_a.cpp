#include "inventory.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace inventory_tools {

[[= scry::reflection::tool{
    "Only translation unit A declares this tool"}]] inline std::string
only_a() {
  return "a";
}

} // namespace inventory_tools

namespace echo_tools {

[[= scry::reflection::tool{"Echo a count"}]] std::int32_t echo(std::int32_t count);

} // namespace echo_tools

// Only this unit sees the name annotation.
[[= scry::reflection::name{"read_from_a"}]] int Tally::read() const { return value; }

scry::Status register_inventory_from_a(scry::ToolRegistry& registry) {
  return registry.add<^^inventory_tools>();
}

scry::Status register_from_a(UnitRegistries& registries) {
  if (auto status = register_inventory_from_a(registries.inventory); !status) {
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
