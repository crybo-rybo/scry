// A binding generates code from the declarations its translation unit can see.
// Two units that see different declarations under one name must each bind what
// they see, whichever order their object files are linked in, so this executable
// is built once per link order. It links Scrydantic alone.

#include "inventory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace echo_tools {
// Each unit declares echo with its own parameter name; this is the definition.
std::int32_t echo(std::int32_t value) { return value; }
} // namespace echo_tools

namespace {

[[nodiscard]] std::vector<std::string>
names_of(const std::vector<scrydantic::BoundTool<>>& tools) {
  std::vector<std::string> names{};
  for (const auto& tool : tools) {
    names.push_back(tool.name);
  }
  return names;
}

// Whether `tools` is echo alone, as one unit declared it: its description, its
// parameter name in the schema, and a handler that decodes that name.
[[nodiscard]] bool is_echo(std::vector<scrydantic::BoundTool<>> tools,
                           const std::string& description,
                           const std::string& parameter) {
  if (tools.size() != 1 || tools.front().name != "echo" ||
      tools.front().description != description) {
    return false;
  }
  const auto expected_schema =
      R"({"additionalProperties":false,"properties":{")" + parameter +
      R"(":{"maximum":2147483647,"minimum":-2147483648,"type":"integer"}},"required":[")" +
      parameter + R"("],"type":"object"})";
  const auto echoed =
      tools.front().invoke(scrydantic::Json{.text = "{\"" + parameter + "\":7}"});
  return tools.front().input_schema.text == expected_schema && echoed &&
         echoed->text == "7";
}

} // namespace

int main() {
  using Names = std::vector<std::string>;
  int failures = 0;
  failures += names_of(bind_inventory_from_a()) != Names{"only_a"} ? 1 : 0;
  failures += names_of(bind_inventory_from_b()) != Names{"only_b"} ? 1 : 0;
  failures += is_echo(bind_echo_from_a(), "Echo a count", "count") ? 0 : 1;
  failures += is_echo(bind_echo_from_b(), "Echo a quantity", "quantity") ? 0 : 1;
  return failures == 0 ? 0 : 1;
}
