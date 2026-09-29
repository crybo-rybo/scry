// add<^^namespace>() registers the tools the calling translation unit can see.
// Two units that see different parts of one namespace must each register their
// own part, whichever order their object files are linked in. This executable is
// built twice, once per link order.

#include "inventory.hpp"

#include <iostream>
#include <string>
#include <vector>

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

} // namespace

int main() {
  scry::ToolRegistry from_a{};
  scry::ToolRegistry from_b{};
  scry::ToolRegistry shared{};
  if (!register_from_a(from_a) || !register_from_b(from_b) ||
      !register_from_a(shared) || !register_from_b(shared)) {
    std::cerr << "a namespace registration failed\n";
    return 1;
  }
  const bool passed = expect_names("unit A", from_a, {"only_a"}) &&
                      expect_names("unit B", from_b, {"only_b"}) &&
                      expect_names("both units", shared, {"only_a", "only_b"});
  return passed ? 0 : 1;
}
