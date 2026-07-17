# Catch2 is a build-time dependency only (ADR 0003); pinned exact tag per
# ADR 0002. SYSTEM keeps its headers outside our warnings-as-errors gate.
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1
    SYSTEM
)
FetchContent_MakeAvailable(Catch2)

add_executable(scry_tests
    version_test.cpp)
target_link_libraries(scry_tests PRIVATE scry::scry scry_warnings Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(scry_tests)
