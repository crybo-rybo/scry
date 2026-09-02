#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <scry/json.hpp>
#include <scry/tool_registry.hpp>

namespace scry_showcase::npc {

struct Position {
  std::int32_t x{};
  std::int32_t y{};

  friend bool operator==(const Position&, const Position&) = default;
};

enum class Direction : std::uint8_t {
  north,
  south,
  east,
  west,
};

enum class NpcTool : std::uint8_t {
  look,
  move_north,
  move_south,
  move_east,
  move_west,
};

using ToolExecutionObserver = std::function<void(NpcTool)>;

class World final {
public:
  static constexpr std::int32_t width = 5;
  static constexpr std::int32_t height = 5;

  [[nodiscard]] Position position() const noexcept;
  [[nodiscard]] scry::Json look() const;
  [[nodiscard]] scry::Json move(Direction direction);

private:
  Position position_{.x = 2, .y = 2};
};

[[nodiscard]] scry::Result<scry::Json> execute_world_tool(World& world, NpcTool tool,
                                                          scry::Json arguments);

// ToolRegistry is additive-only. Call this with a registry that does not
// already contain any showcase tool name; a later-name collision can leave
// earlier registrations installed.
[[nodiscard]] scry::Status register_world_tools(scry::ToolRegistry& registry,
                                                std::shared_ptr<World> world,
                                                ToolExecutionObserver observer = {});

} // namespace scry_showcase::npc
