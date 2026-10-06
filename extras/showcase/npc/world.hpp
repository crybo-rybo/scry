#pragma once

#include <cstdint>
#include <optional>
#include <scry/annotations.hpp>
#include <string>
#include <vector>

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

struct Bounds {
  std::int32_t width{};
  std::int32_t height{};
};

// What look reports. Scry encodes it from these members, so the model reads
// {"available_moves":["east",...],"bounds":{...},"position":{...}}.
struct Observation {
  std::vector<Direction> available_moves{};
  Bounds bounds{};
  Position position{};
};

// What a move reports. A move that stays inside the grid has no reason, and
// skip_null leaves the member out rather than writing null.
struct MoveOutcome {
  Direction direction{};
  bool moved{};
  Position position{};
  [[= scry::reflection::skip_null]] std::optional<std::string> reason{};
};

// The NPC's world and its tools. The world is a toolbox: registering it with
// ToolRegistry::add() makes each annotated member function a tool named after
// the function, bound to this object, with a schema generated from its
// parameters (none, here) and a result encoded from its return type. Tools run
// on the host thread inside Harness::update(), so the state needs no locking.
class World final {
public:
  static constexpr std::int32_t width = 5;
  static constexpr std::int32_t height = 5;

  [[nodiscard]] Position position() const noexcept;

  [[= scry::reflection::tool{
      "Observe the NPC position, bounds, and available moves."}]] Observation
      look() const;

  [[= scry::reflection::tool{"Move the NPC one cell north."}]] MoveOutcome move_north();

  [[= scry::reflection::tool{"Move the NPC one cell south."}]] MoveOutcome move_south();

  [[= scry::reflection::tool{"Move the NPC one cell east."}]] MoveOutcome move_east();

  [[= scry::reflection::tool{"Move the NPC one cell west."}]] MoveOutcome move_west();

  MoveOutcome move(Direction direction);

private:
  Position position_{.x = 2, .y = 2};
};

} // namespace scry_showcase::npc
