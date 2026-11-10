// A registration generates code from the declarations its translation unit can
// see. Two units that see different declarations under one name must each
// register what they see, whichever order their object files are linked in, so
// this executable is built once per link order.

#include "inventory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace echo_tools {
// Each unit declares echo with its own parameter name; this is the definition.
std::int32_t echo(std::int32_t value) { return value; }
} // namespace echo_tools

namespace {

// The manifest of a registry holding only echo, as one unit declared it.
[[nodiscard]] std::string echo_manifest(const std::string& description,
                                        const std::string& parameter) {
  return R"({"tools":[{"description":")" + description +
         R"(","input_schema":{"additionalProperties":false,"properties":{")" +
         parameter +
         R"(":{"maximum":2147483647,"minimum":-2147483648,"type":"integer"}},"required":[")" +
         parameter + R"("],"type":"object"},"name":"echo"}],"version":1})";
}

[[nodiscard]] bool manifest_is(const scry::ToolRegistry& registry,
                               const std::string& expected) {
  const auto manifest = registry.to_json();
  return manifest && manifest->text == expected;
}

} // namespace

int main() {
  scry::ToolRegistry inventory_a{};
  scry::ToolRegistry inventory_b{};
  scry::ToolRegistry both{};
  scry::ToolRegistry echo_a{};
  scry::ToolRegistry echo_b{};
  if (!register_inventory_from_a(inventory_a) ||
      !register_inventory_from_b(inventory_b) || !register_inventory_from_a(both) ||
      !register_inventory_from_b(both) || !register_echo_from_a(echo_a) ||
      !register_echo_from_b(echo_b)) {
    return 1;
  }
  using Names = std::vector<std::string>;
  int failures = 0;
  failures += inventory_a.names() != Names{"only_a"} ? 1 : 0;
  failures += inventory_b.names() != Names{"only_b"} ? 1 : 0;
  failures += both.names() != Names{"only_a", "only_b"} ? 1 : 0;
  failures += manifest_is(echo_a, echo_manifest("Echo a count", "count")) ? 0 : 1;
  failures += manifest_is(echo_b, echo_manifest("Echo a quantity", "quantity")) ? 0 : 1;
  return failures == 0 ? 0 : 2;
}
