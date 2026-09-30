#pragma once

#include <scry/tool_registry.hpp>

// Two translation units register tools under one name while seeing different
// declarations of them. Each must register what it sees, whichever order the
// units are linked in.
//
// - Namespace inventory_tools: each unit declares a different part of it.
// - Function echo_tools::echo: each unit declares it with its own parameter name
//   and description; main.cpp defines it.
// - Toolbox Tally: unit A defines Tally::read with a name annotation that unit B
//   never sees.
namespace inventory_tools {}

struct Tally {
  [[= scry::reflection::tool{"Read the tally"}]] int read() const;
  int value{};
};

// Each unit registers into the registry of the matching field.
struct UnitRegistries {
  scry::ToolRegistry inventory;
  scry::ToolRegistry echo;
  scry::ToolRegistry shared_toolbox;
  scry::ToolRegistry owned_toolbox;
};

[[nodiscard]] scry::Status register_from_a(UnitRegistries& registries);
[[nodiscard]] scry::Status register_from_b(UnitRegistries& registries);
[[nodiscard]] scry::Status register_inventory_from_a(scry::ToolRegistry& registry);
[[nodiscard]] scry::Status register_inventory_from_b(scry::ToolRegistry& registry);
