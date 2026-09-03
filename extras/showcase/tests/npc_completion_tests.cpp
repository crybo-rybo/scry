#include "completion_validation.hpp"

#include <catch2/catch_test_macros.hpp>
#include <scry/scry.hpp>

TEST_CASE("NPC completion validation rejects false-success terminal states") {
  const scry::Completion completed{
      .text = "The NPC moved.",
      .finish_reason = scry::FinishReason::completed,
  };
  CHECK(scry_showcase::npc::completion_failure(completed, 1).empty());

  auto invalid = completed;
  invalid.finish_reason = scry::FinishReason::length;
  CHECK(scry_showcase::npc::completion_failure(invalid, 1) ==
        "Model response did not complete normally");

  CHECK(scry_showcase::npc::completion_failure(completed, 0) ==
        "Model completed without executing an NPC tool");

  invalid = completed;
  invalid.text.clear();
  CHECK(scry_showcase::npc::completion_failure(invalid, 1) ==
        "Model completed without a final answer");
}
