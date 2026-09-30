// A registration generates code from the declarations its translation unit can
// see: the tools of a namespace, and a tool function's name, description, and
// parameter names. Two units that see different declarations under one name must
// each register what they see, whichever order their object files are linked in.
// This executable is built twice, once per link order.

#include "inventory.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace echo_tools {

// Each unit declares echo with its own parameter name; this is the definition.
std::int32_t echo(std::int32_t value) { return value; }

} // namespace echo_tools

namespace {

[[nodiscard]] bool expect_names(const char* label, const scry::ToolRegistry& registry,
                                const std::vector<std::string>& expected) {
  const auto names = registry.names();
  if (names == expected) {
    return true;
  }
  std::cerr << label << ": registered";
  for (const auto& name : names) {
    std::cerr << ' ' << name;
  }
  std::cerr << " but expected";
  for (const auto& name : expected) {
    std::cerr << ' ' << name;
  }
  std::cerr << '\n';
  return false;
}

// The manifest of a registry holding only echo, as one unit declared it.
[[nodiscard]] std::string echo_manifest(const std::string& description,
                                        const std::string& parameter) {
  return R"({"tools":[{"description":")" + description +
         R"(","input_schema":{"additionalProperties":false,"properties":{")" +
         parameter +
         R"(":{"maximum":2147483647,"minimum":-2147483648,"type":"integer"}},"required":[")" +
         parameter + R"("],"type":"object"},"name":"echo"}],"version":1})";
}

[[nodiscard]] bool expect_manifest(const char* label,
                                   const scry::ToolRegistry& registry,
                                   const std::string& expected) {
  const auto manifest = registry.to_json();
  if (manifest && manifest->text == expected) {
    return true;
  }
  std::cerr << label << ": exported "
            << (manifest ? manifest->text : std::string{"an error"}) << " but expected "
            << expected << '\n';
  return false;
}

[[nodiscard]] bool expect_unit(const char* label, const UnitRegistries& registries,
                               const std::vector<std::string>& inventory,
                               const std::string& echo, const std::string& toolbox) {
  // Every check runs, so a failure reports each registration that went wrong.
  const bool inventory_ok = expect_names(label, registries.inventory, inventory);
  const bool echo_ok = expect_manifest(label, registries.echo, echo);
  const bool shared_ok = expect_names(label, registries.shared_toolbox, {toolbox});
  const bool owned_ok = expect_names(label, registries.owned_toolbox, {toolbox});
  return inventory_ok && echo_ok && shared_ok && owned_ok;
}

} // namespace

int main() {
  UnitRegistries from_a{};
  UnitRegistries from_b{};
  scry::ToolRegistry both{};
  if (!register_from_a(from_a) || !register_from_b(from_b) ||
      !register_inventory_from_a(both) || !register_inventory_from_b(both)) {
    std::cerr << "a registration failed\n";
    return 1;
  }
  const bool unit_a =
      expect_unit("unit A", from_a, {"only_a"}, echo_manifest("Echo a count", "count"),
                  "read_from_a");
  const bool unit_b = expect_unit("unit B", from_b, {"only_b"},
                                  echo_manifest("Echo a quantity", "quantity"), "read");
  const bool shared = expect_names("both units", both, {"only_a", "only_b"});
  return unit_a && unit_b && shared ? 0 : 1;
}
