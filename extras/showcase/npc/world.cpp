#include "world.hpp"

#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

namespace scry_showcase::npc {
namespace {

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

} // namespace

Position World::position() const noexcept { return position_; }

Observation World::look() const {
  Observation observation{
      .bounds = {.width = width, .height = height},
      .position = position_,
  };
  for (const auto direction :
       {Direction::east, Direction::north, Direction::south, Direction::west}) {
    if (is_inside(candidate_position(position_, direction))) {
      observation.available_moves.push_back(direction);
    }
  }
  return observation;
}

MoveOutcome World::move_north() { return move(Direction::north); }

MoveOutcome World::move_south() { return move(Direction::south); }

MoveOutcome World::move_east() { return move(Direction::east); }

MoveOutcome World::move_west() { return move(Direction::west); }

MoveOutcome World::move(const Direction direction) {
  const auto candidate = candidate_position(position_, direction);
  const bool moved = is_inside(candidate);
  if (moved) {
    position_ = candidate;
  }
  return {
      .direction = direction,
      .moved = moved,
      .position = position_,
      .reason = moved ? std::nullopt : std::optional<std::string>{"boundary"},
  };
}

} // namespace scry_showcase::npc
