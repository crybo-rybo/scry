#pragma once

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

// Provider golden fixtures. SCRY_ANTHROPIC_FIXTURE_DIR and
// SCRY_OPENAI_FIXTURE_DIR are set by tests/provider/CMakeLists.txt; see
// tests/fixtures/README.md for where these payloads come from and how to
// re-capture them.
namespace scry::test_fixtures {

[[nodiscard]] inline std::string fixture(const std::string_view directory,
                                         const std::string_view name) {
  std::ifstream input{std::string{directory} + "/" + std::string{name}};
  REQUIRE(input.good());
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] inline std::string anthropic_fixture(const std::string_view name) {
  return fixture(SCRY_ANTHROPIC_FIXTURE_DIR, name);
}

[[nodiscard]] inline std::string openai_fixture(const std::string_view name) {
  return fixture(SCRY_OPENAI_FIXTURE_DIR, name);
}

} // namespace scry::test_fixtures
