#pragma once

#include <scrydantic/tools.hpp>
#include <vector>

// Two translation units bind tools under one name while seeing different
// declarations: each declares a different part of namespace inventory_tools, and
// declares echo_tools::echo with its own parameter name and description.
namespace inventory_tools {}

[[nodiscard]] std::vector<scrydantic::BoundTool<>> bind_inventory_from_a();
[[nodiscard]] std::vector<scrydantic::BoundTool<>> bind_inventory_from_b();
[[nodiscard]] std::vector<scrydantic::BoundTool<>> bind_echo_from_a();
[[nodiscard]] std::vector<scrydantic::BoundTool<>> bind_echo_from_b();
