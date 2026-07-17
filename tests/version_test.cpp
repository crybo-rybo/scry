#include <scry/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("version constants match the reported version string", "[version]") {
    // The constants live in the header, the string comes from the CMake
    // project version — this cross-check catches the two drifting apart.
    auto const expected = std::to_string(scry::version_major) + '.' +
                          std::to_string(scry::version_minor) + '.' +
                          std::to_string(scry::version_patch);
    CHECK(scry::version_string() == expected);
}
