#include "world.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <scry/scry.hpp>
#include <utility>

using scry_showcase::npc::Direction;
using scry_showcase::npc::NpcTool;
using scry_showcase::npc::Position;
using scry_showcase::npc::World;

TEST_CASE("NPC world begins at the center with a canonical observation") {
  const World world;

  CHECK(world.position() == Position{.x = 2, .y = 2});
  CHECK(
      world.look().text ==
      R"({"available_moves":["east","north","south","west"],"bounds":{"height":5,"width":5},"position":{"x":2,"y":2}})");
}

TEST_CASE("NPC movement updates position and returns canonical results") {
  World world;

  CHECK(world.move(Direction::north).text ==
        R"({"direction":"north","moved":true,"position":{"x":2,"y":1}})");
  CHECK(world.move(Direction::west).text ==
        R"({"direction":"west","moved":true,"position":{"x":1,"y":1}})");
  CHECK(world.move(Direction::south).text ==
        R"({"direction":"south","moved":true,"position":{"x":1,"y":2}})");
  CHECK(world.move(Direction::east).text ==
        R"({"direction":"east","moved":true,"position":{"x":2,"y":2}})");
}

TEST_CASE("NPC movement cannot leave the bounded grid") {
  World world;

  static_cast<void>(world.move(Direction::north));
  static_cast<void>(world.move(Direction::north));
  CHECK(world.position() == Position{.x = 2, .y = 0});
  CHECK(
      world.move(Direction::north).text ==
      R"({"direction":"north","moved":false,"position":{"x":2,"y":0},"reason":"boundary"})");

  static_cast<void>(world.move(Direction::west));
  static_cast<void>(world.move(Direction::west));
  CHECK(
      world.move(Direction::west).text ==
      R"({"direction":"west","moved":false,"position":{"x":0,"y":0},"reason":"boundary"})");
  CHECK(
      world.look().text ==
      R"({"available_moves":["east","south"],"bounds":{"height":5,"width":5},"position":{"x":0,"y":0}})");

  for (int step = 0; step < 4; ++step) {
    static_cast<void>(world.move(Direction::east));
  }
  CHECK(
      world.move(Direction::east).text ==
      R"({"direction":"east","moved":false,"position":{"x":4,"y":0},"reason":"boundary"})");

  for (int step = 0; step < 4; ++step) {
    static_cast<void>(world.move(Direction::south));
  }
  CHECK(
      world.move(Direction::south).text ==
      R"({"direction":"south","moved":false,"position":{"x":4,"y":4},"reason":"boundary"})");
  CHECK(
      world.look().text ==
      R"({"available_moves":["north","west"],"bounds":{"height":5,"width":5},"position":{"x":4,"y":4}})");
}

TEST_CASE("every NPC tool rejects nonempty arguments without mutating the world") {
  World world;
  constexpr std::array tools{
      NpcTool::look,      NpcTool::move_north, NpcTool::move_south,
      NpcTool::move_east, NpcTool::move_west,
  };

  for (const auto tool : tools) {
    const auto result = scry_showcase::npc::execute_world_tool(
        world, tool, scry::Json{.text = R"({"unexpected":true})"});
    REQUIRE_FALSE(result);
    CHECK(result.error().category == scry::ErrorCategory::tool);
  }
  CHECK(world.position() == Position{.x = 2, .y = 2});
}

TEST_CASE("NPC tool execution accepts the canonical empty object") {
  World world;

  const auto observation = scry_showcase::npc::execute_world_tool(
      world, NpcTool::look, scry::Json{.text = "{}"});
  REQUIRE(observation);
  CHECK(observation->text == world.look().text);

  const auto movement = scry_showcase::npc::execute_world_tool(
      world, NpcTool::move_east, scry::Json{.text = "{}"});
  REQUIRE(movement);
  CHECK(movement->text ==
        R"({"direction":"east","moved":true,"position":{"x":3,"y":2}})");
}

TEST_CASE("NPC tools register through the public app-thread tool boundary") {
  auto harness_result = scry::Harness::create({
      .base_url = "http://127.0.0.1:1/v1",
      .model = "not-contacted",
      .dialect = scry::ProviderDialect::openai_compatible,
  });
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  auto world = std::make_shared<World>();
  REQUIRE(scry_showcase::npc::register_world_tools(harness.tools(), world));
  CHECK(harness.tools().size() == 5);

  const auto duplicate =
      scry_showcase::npc::register_world_tools(harness.tools(), world);
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().category == scry::ErrorCategory::invalid_state);
  CHECK(harness.tools().size() == 5);
}

TEST_CASE("NPC registration documents additive partial failure") {
  auto harness_result = scry::Harness::create({
      .base_url = "http://127.0.0.1:1/v1",
      .model = "not-contacted",
      .dialect = scry::ProviderDialect::openai_compatible,
  });
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  REQUIRE(harness.tools().add(
      {
          .name = "move_south",
          .description = "Intentional collision",
          .input_schema = {.text = "{}"},
      },
      [](scry::Json) -> scry::Result<scry::Json> { return scry::Json{.text = "{}"}; }));

  const auto registration = scry_showcase::npc::register_world_tools(
      harness.tools(), std::make_shared<World>());
  REQUIRE_FALSE(registration);
  CHECK(registration.error().category == scry::ErrorCategory::invalid_state);
  CHECK(harness.tools().size() == 3);
}

TEST_CASE("NPC tool registration rejects a missing world") {
  auto harness_result = scry::Harness::create({
      .base_url = "http://127.0.0.1:1/v1",
      .model = "not-contacted",
      .dialect = scry::ProviderDialect::openai_compatible,
  });
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  const auto registration = scry_showcase::npc::register_world_tools(
      harness.tools(), std::shared_ptr<World>{});
  REQUIRE_FALSE(registration);
  CHECK(registration.error().category == scry::ErrorCategory::invalid_state);
  CHECK(harness.tools().empty());
}
