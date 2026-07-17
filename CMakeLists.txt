cmake_minimum_required(VERSION 3.25)

project(scry
    VERSION 0.1.0
    DESCRIPTION "A C++ LLM harness for applications with their own main loops"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# TOOL-003 / PORT-002: the P2996 reflection layer is severable. Until the M3
# layer lands, this flag proves the toolchain accepts C++26 mode (and the
# reflection flag where the compiler has one) — the CI matrix builds both ways.
option(SCRY_ENABLE_REFLECTION "Build the C++26 (P2996) reflection layer" OFF)
option(SCRY_BUILD_TESTS "Build the scry test suite" ${PROJECT_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------
# Language standard — one standard for the whole build, dependencies included
# ---------------------------------------------------------------------------
# FetchContent'd deps must compile with the same standard as our TUs:
# feature-detection macros differ per standard, so e.g. Catch2 built as its
# default C++14 never compiles StringMaker<string_view>, which our C++23 test
# TUs then fail to link against.
if(SCRY_ENABLE_REFLECTION)
    set(CMAKE_CXX_STANDARD 26)
else()
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Warnings — QA-006: exactly this set, as errors, across the whole matrix
# ---------------------------------------------------------------------------
add_library(scry_warnings INTERFACE)
target_compile_options(scry_warnings INTERFACE
    -Wall -Wextra -Wconversion -Wshadow -Werror)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(scry)
add_library(scry::scry ALIAS scry)

target_sources(scry PRIVATE src/version.cpp)
target_include_directories(scry PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(scry PUBLIC cxx_std_23)
target_link_libraries(scry PRIVATE scry_warnings)
target_compile_definitions(scry PRIVATE SCRY_VERSION_STRING="${PROJECT_VERSION}")

if(SCRY_ENABLE_REFLECTION)
    target_compile_features(scry PUBLIC cxx_std_26)
    target_compile_definitions(scry PUBLIC SCRY_HAS_REFLECTION=1)

    # clang-p2996 gates P2996 + P3394 behind -freflection-latest; GCC trunk
    # needs no extra flag as of mid-2026. Probe rather than hardcode.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-freflection-latest SCRY_CXX_HAS_FREFLECTION_LATEST)
    if(SCRY_CXX_HAS_FREFLECTION_LATEST)
        target_compile_options(scry PUBLIC -freflection-latest)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SCRY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
