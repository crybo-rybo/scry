#pragma once

#include <scry/tool_registry.hpp>

// Each translation unit declares a different part of namespace inventory_tools
// and registers it with add<^^inventory_tools>(). The namespace is open, so the
// two units see different tool sets under one template argument.
namespace inventory_tools {}

[[nodiscard]] scry::Status register_from_a(scry::ToolRegistry& registry);
[[nodiscard]] scry::Status register_from_b(scry::ToolRegistry& registry);
