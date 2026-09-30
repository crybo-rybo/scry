#pragma once

#include <scry/tool_registry.hpp>

// Two translation units register tools under one name while seeing different
// declarations: each declares a different part of namespace inventory_tools, and
// declares echo_tools::echo with its own parameter name and description.
namespace inventory_tools {}

[[nodiscard]] scry::Status register_inventory_from_a(scry::ToolRegistry& registry);
[[nodiscard]] scry::Status register_inventory_from_b(scry::ToolRegistry& registry);
[[nodiscard]] scry::Status register_echo_from_a(scry::ToolRegistry& registry);
[[nodiscard]] scry::Status register_echo_from_b(scry::ToolRegistry& registry);
