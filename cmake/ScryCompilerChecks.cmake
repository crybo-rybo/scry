if(SCRY_CLANG_TOOLING_LIBCXX AND NOT SCRY_CLANG_TOOLING)
  message(
    FATAL_ERROR
    "SCRY_CLANG_TOOLING_LIBCXX selects the standard library for the "
    "SCRY_CLANG_TOOLING build; enable SCRY_CLANG_TOOLING or leave it OFF"
  )
endif()

if(SCRY_BUILD_FUZZERS AND
   (NOT SCRY_CLANG_TOOLING OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
  message(FATAL_ERROR
    "SCRY_BUILD_FUZZERS requires SCRY_CLANG_TOOLING=ON and a Clang-family compiler")
endif()

# Scry's public API is C++26: the reflected typed-tool surface is part of the
# library, not an option. The implementation under src/ is deliberately kept to
# portable C++23 with no reflection syntax so Clang tooling — clang-tidy and
# libFuzzer — can still compile it; SCRY_CLANG_TOOLING selects exactly that
# build and nothing a consumer should ever link against.
if(SCRY_CLANG_TOOLING)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(
      FATAL_ERROR
      "SCRY_CLANG_TOOLING builds the implementation with a Clang-family "
      "compiler; ${CMAKE_CXX_COMPILER_ID} was selected"
    )
  endif()
  # Ordinary tests and examples use the GCC-only public reflection API.
  # Fuzz targets are registered separately and use private C++23 headers.
  set(SCRY_BUILD_EXAMPLES OFF)
  set(SCRY_BUILD_TESTS OFF)

  # The implementation uses std::jthread and std::stop_token. libstdc++ and
  # recent libc++ expose that C++20 surface directly; LLVM 18's libc++ still
  # gates it behind experimental-library mode. Probe rather than assume, so the
  # flag appears only on the standard libraries that need it.
  include(CheckCXXSourceCompiles)
  set(SCRY_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "-std=c++23")
  if(SCRY_CLANG_TOOLING_LIBCXX)
    # The probe must see the same standard library the build will use, or the
    # -fexperimental-library decision below is made against the wrong one.
    string(APPEND CMAKE_REQUIRED_FLAGS " -stdlib=libc++")
  endif()
  check_cxx_source_compiles(
    [=[
      #include <stop_token>
      int main() {
        std::stop_source source;
        return source.get_token().stop_requested() ? 1 : 0;
      }
    ]=]
    SCRY_TOOLING_HAS_STOP_TOKEN
  )
  set(CMAKE_REQUIRED_FLAGS "${SCRY_SAVED_REQUIRED_FLAGS}")
else()
  if(
    NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16
  )
    message(
      FATAL_ERROR
      "Scry requires GCC 16 or newer: its public API uses C++26 reflection "
      "(P2996). Set SCRY_CLANG_TOOLING=ON only for clang-tidy or fuzzing "
      "builds of the implementation."
    )
  endif()

  include(CheckCXXSourceCompiles)
  set(SCRY_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "-std=c++26 -freflection")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/probes/reflection.cpp")
  file(READ "${CMAKE_CURRENT_LIST_DIR}/probes/reflection.cpp" SCRY_REFLECTION_PROBE)
  unset(SCRY_COMPILER_HAS_REFLECTION_ANNOTATIONS CACHE)
  check_cxx_source_compiles(
    "${SCRY_REFLECTION_PROBE}"
    SCRY_COMPILER_HAS_REFLECTION_ANNOTATIONS
  )
  set(CMAKE_REQUIRED_FLAGS "${SCRY_SAVED_REQUIRED_FLAGS}")
  if(NOT SCRY_COMPILER_HAS_REFLECTION_ANNOTATIONS)
    message(
      FATAL_ERROR
      "The selected compiler cannot compile Scry's required P2996/P3394 "
      "reflection annotation-query surface"
    )
  endif()
endif()
