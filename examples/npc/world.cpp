#include "world.hpp"

#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace scry_showcase::npc {
namespace {

constexpr std::string_view empty_object_schema =
    R"({"type":"object","properties":{},"additionalProperties":false})";

struct ToolDescriptor {
  NpcTool tool;
  std::string_view name;
  std::string_view description;
};

constexpr std::array tool_descriptors{
    ToolDescriptor{NpcTool::look, "look",
                   "Observe the NPC position, bounds, and available moves."},
    ToolDescriptor{NpcTool::move_north, "move_north", "Move the NPC one cell north."},
    ToolDescriptor{NpcTool::move_south, "move_south", "Move the NPC one cell south."},
    ToolDescriptor{NpcTool::move_east, "move_east", "Move the NPC one cell east."},
    ToolDescriptor{NpcTool::move_west, "move_west", "Move the NPC one cell west."},
};

[[nodiscard]] std::string_view direction_name(const Direction direction) {
  switch (direction) {
  case Direction::north:
    return "north";
  case Direction::south:
    return "south";
  case Direction::east:
    return "east";
  case Direction::west:
    return "west";
  }
  std::unreachable();
}

[[nodiscard]] Position candidate_position(const Position current,
                                          const Direction direction) {
  switch (direction) {
  case Direction::north:
    return {.x = current.x, .y = current.y - 1};
  case Direction::south:
    return {.x = current.x, .y = current.y + 1};
  case Direction::east:
    return {.x = current.x + 1, .y = current.y};
  case Direction::west:
    return {.x = current.x - 1, .y = current.y};
  }
  std::unreachable();
}

[[nodiscard]] bool is_inside(const Position position) noexcept {
  return position.x >= 0 && position.x < World::width && position.y >= 0 &&
         position.y < World::height;
}

void append_quoted_move(std::string& json, const std::string_view move,
                        bool& has_move) {
  if (has_move) {
    json.push_back(',');
  }
  json.push_back('"');
  json.append(move);
  json.push_back('"');
  has_move = true;
}

[[nodiscard]] std::string available_moves_json(const Position position) {
  std::string json{"["};
  bool has_move = false;
  if (position.x + 1 < World::width) {
    append_quoted_move(json, "east", has_move);
  }
  if (position.y > 0) {
    append_quoted_move(json, "north", has_move);
  }
  if (position.y + 1 < World::height) {
    append_quoted_move(json, "south", has_move);
  }
  if (position.x > 0) {
    append_quoted_move(json, "west", has_move);
  }
  json.push_back(']');
  return json;
}

[[nodiscard]] std::string position_json(const Position position) {
  return R"({"x":)" + std::to_string(position.x) + R"(,"y":)" +
         std::to_string(position.y) + "}";
}

[[nodiscard]] scry::ToolDefinition tool_definition(std::string name,
                                                   std::string description) {
  return {
      .name = std::move(name),
      .description = std::move(description),
      .input_schema = {.text = std::string{empty_object_schema}},
  };
}

[[nodiscard]] std::string_view tool_name(const NpcTool tool) {
  switch (tool) {
  case NpcTool::look:
    return "look";
  case NpcTool::move_north:
    return "move_north";
  case NpcTool::move_south:
    return "move_south";
  case NpcTool::move_east:
    return "move_east";
  case NpcTool::move_west:
    return "move_west";
  }
  std::unreachable();
}

[[nodiscard]] scry::Status validate_arguments(const scry::Json& arguments,
                                              const NpcTool tool) {
  if (arguments.text == "{}") {
    return {};
  }
  return std::unexpected(scry::Error{
      .category = scry::ErrorCategory::tool,
      .message = std::string{tool_name(tool)} + " expects an empty object",
  });
}

[[nodiscard]] scry::ToolHandler tool_handler(std::shared_ptr<World> world,
                                             const NpcTool tool,
                                             ToolExecutionObserver observer) {
  return [world = std::move(world), tool, observer = std::move(observer)](
             scry::Json arguments) -> scry::Result<scry::Json> {
    if (observer) {
      observer(tool);
    }
    return execute_world_tool(*world, tool, std::move(arguments));
  };
}

} // namespace

Position World::position() const noexcept { return position_; }

scry::Json World::look() const {
  return {
      .text = R"({"available_moves":)" + available_moves_json(position_) +
              R"(,"bounds":{"height":)" + std::to_string(height) + R"(,"width":)" +
              std::to_string(width) + R"(},"position":)" + position_json(position_) +
              "}",
  };
}

scry::Json World::move(const Direction direction) {
  const auto candidate = candidate_position(position_, direction);
  const bool moved = is_inside(candidate);
  if (moved) {
    position_ = candidate;
  }

  auto json = R"({"direction":)" + std::string{"\""} +
              std::string{direction_name(direction)} + R"(","moved":)" +
              (moved ? "true" : "false") + R"(,"position":)" + position_json(position_);
  if (!moved) {
    json += R"(,"reason":"boundary")";
  }
  json.push_back('}');
  return {.text = std::move(json)};
}

[[nodiscard]] scry::Result<scry::Json>
execute_world_tool(World& world, const NpcTool tool, scry::Json arguments) {
  if (auto validation = validate_arguments(arguments, tool); !validation) {
    return std::unexpected(std::move(validation.error()));
  }

  switch (tool) {
  case NpcTool::look:
    return world.look();
  case NpcTool::move_north:
    return world.move(Direction::north);
  case NpcTool::move_south:
    return world.move(Direction::south);
  case NpcTool::move_east:
    return world.move(Direction::east);
  case NpcTool::move_west:
    return world.move(Direction::west);
  }
  std::unreachable();
}

scry::Status register_world_tools(scry::ToolRegistry& registry,
                                  std::shared_ptr<World> world,
                                  ToolExecutionObserver observer) {
  if (!world) {
    return std::unexpected(scry::Error{
        .category = scry::ErrorCategory::invalid_state,
        .message = "register_world_tools requires a world",
    });
  }

  for (const auto& descriptor : tool_descriptors) {
    auto status = registry.add(tool_definition(std::string{descriptor.name},
                                               std::string{descriptor.description}),
                               tool_handler(world, descriptor.tool, observer),
                               {.execution = scry::ToolExecution::app_thread});
    if (!status) {
      return status;
    }
  }
  return {};
}

} // namespace scry_showcase::npc
