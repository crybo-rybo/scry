cmake_minimum_required(VERSION 3.28)

project(
  scry
  VERSION 0.2.0
  DESCRIPTION "A main-loop-friendly C++ LLM harness"
  LANGUAGES CXX
)

# The CMake project version above is the only place the release number lives;
# the public <scry/version.hpp> is generated from it so the two cannot drift.
set(
  SCRY_GENERATED_INCLUDE_DIR
  "${PROJECT_BINARY_DIR}/generated/include"
)
set(
  SCRY_GENERATED_VERSION_HEADER
  "${SCRY_GENERATED_INCLUDE_DIR}/scry/version.hpp"
)
configure_file(
  cmake/version.hpp.in
  "${SCRY_GENERATED_VERSION_HEADER}"
  @ONLY
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CMakePackageConfigHelpers)
include(FetchContent)
include(GNUInstallDirs)
include(ScryFuzz)

option(SCRY_BUILD_TESTS "Build Scry's tests" ${PROJECT_IS_TOP_LEVEL})
option(SCRY_BUILD_EXAMPLES "Compile Scry's canonical example" ${PROJECT_IS_TOP_LEVEL})
option(SCRY_ENABLE_FORMAT_CHECK "Create clang-format check and fix targets" OFF)
option(SCRY_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(SCRY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(
  SCRY_CLANG_TOOLING
  "Build only the portable C++23 implementation with a Clang compiler, for \
clang-tidy and libFuzzer. Not a supported consumer build."
  OFF
)
option(
  SCRY_CLANG_TOOLING_LIBCXX
  "Use libc++ for the Clang tooling build; needed where the host's libstdc++ \
lacks std::expected for the tooling compiler"
  OFF
)
option(SCRY_BUILD_FUZZERS "Build short protocol fuzz targets" OFF)

if(SCRY_CLANG_TOOLING_LIBCXX AND NOT SCRY_CLANG_TOOLING)
  message(
    FATAL_ERROR
    "SCRY_CLANG_TOOLING_LIBCXX selects the standard library for the "
    "SCRY_CLANG_TOOLING build; enable SCRY_CLANG_TOOLING or leave it OFF"
  )
endif()

set(
  SCRY_SANITIZER
  "none"
  CACHE STRING
  "Sanitizer mode: none, address-undefined, or thread"
)
set_property(
  CACHE SCRY_SANITIZER
  PROPERTY STRINGS none address-undefined thread
)

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
  # <scry/scry.hpp> pulls in the reflected public API, which no Clang can
  # compile, so this mode builds the library and nothing that includes the
  # umbrella. The example is always out; tests/ is entered only to register the
  # fuzz targets, which reach the implementation through private headers.
  set(SCRY_BUILD_EXAMPLES OFF)
  if(NOT SCRY_BUILD_FUZZERS)
    set(SCRY_BUILD_TESTS OFF)
  endif()

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
  unset(SCRY_COMPILER_HAS_REFLECTION_ANNOTATIONS CACHE)
  check_cxx_source_compiles(
    [=[
      #include <cstddef>
      #include <meta>
      #if !defined(__cpp_impl_reflection)
      #error P2996 feature macro is missing
      #endif

      template <std::size_t Size> struct probe_description {
        char text[Size]{};
        consteval probe_description(const char (&value)[Size]) {
          for (std::size_t index = 0; index < Size; ++index) {
            text[index] = value[index];
          }
        }
      };

      struct probe_arguments {
        [[= probe_description{"probe"}]] int value{};
      };

      consteval bool has_required_annotation_queries() {
        constexpr auto members = std::define_static_array(
          std::meta::nonstatic_data_members_of(
            ^^probe_arguments, std::meta::access_context::unchecked()));
        constexpr auto annotations = std::define_static_array(
          std::meta::annotations_of(members[0]));
        if constexpr (annotations.size() != 1) {
          return false;
        }
        constexpr auto annotation = annotations[0];
        if (!std::meta::is_annotation(annotation)) {
          return false;
        }
        const auto type = std::meta::type_of(annotation);
        if (!std::meta::has_template_arguments(type) ||
            std::meta::template_of(type) != ^^probe_description) {
          return false;
        }
        using Annotation = [:std::meta::type_of(annotation):];
        constexpr auto payload = std::meta::extract<Annotation>(annotation);
        return payload.text[0] == 'p';
      }

      static_assert(has_required_annotation_queries());
      int main() { return 0; }
    ]=]
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

find_package(CURL 7.84 REQUIRED)
find_package(Threads REQUIRED)

# FIND_PACKAGE_ARGS lets a packaged Glaze (vcpkg, Conan, Homebrew) satisfy the
# dependency; the pinned fetch is the fallback when find_package(glaze) fails.
FetchContent_Declare(
  glaze
  GIT_REPOSITORY https://github.com/stephenberry/glaze.git
  GIT_TAG 8b60d82c66311c145c4d03be3b556b555a9cb111
  SYSTEM
  EXCLUDE_FROM_ALL
  FIND_PACKAGE_ARGS
)
FetchContent_MakeAvailable(glaze)

# Scry consumes Glaze privately through raw include directories so no
# glaze::glaze link dependency is ever exported. Where those headers live
# depends on which of the two paths above provided them.
if(glaze_SOURCE_DIR)
  set(SCRY_GLAZE_INCLUDE_DIR "${glaze_SOURCE_DIR}/include")
else()
  get_target_property(
    SCRY_GLAZE_INCLUDE_DIR
    glaze::glaze
    INTERFACE_INCLUDE_DIRECTORIES
  )
endif()
if(NOT SCRY_GLAZE_INCLUDE_DIR)
  message(FATAL_ERROR "Could not determine the Glaze include directory")
endif()

add_library(
  scry
  STATIC
    src/core/error.cpp
    src/core/retry.cpp
    src/core/json_codec.cpp
    src/core/json_view.cpp
    src/reflection/json_bridge.cpp
    src/runtime/config.cpp
    src/runtime/conversation.cpp
    src/runtime/conversation_persistence.cpp
    src/runtime/harness.cpp
    src/runtime/messages.cpp
    src/runtime/pump.cpp
    src/runtime/pump_state.cpp
    src/runtime/queue.cpp
    src/runtime/tool_registry.cpp
    src/runtime/tool_dispatch.cpp
    src/runtime/turn.cpp
    src/runtime/worker.cpp
)
add_library(scry::scry ALIAS scry)
set_target_properties(
  scry
  PROPERTIES
    EXPORT_NAME scry
    POSITION_INDEPENDENT_CODE ON
)
if(SCRY_CLANG_TOOLING)
  target_compile_features(scry PUBLIC cxx_std_23)
else()
  target_compile_features(scry PUBLIC cxx_std_26)
  target_compile_options(scry PUBLIC -freflection)
  set_target_properties(
    scry
    PROPERTIES
      CXX_EXTENSIONS OFF
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
  )
endif()
target_include_directories(
  scry
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${SCRY_GENERATED_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_include_directories(scry SYSTEM PRIVATE "${SCRY_GLAZE_INCLUDE_DIR}")

add_library(scry_project_options INTERFACE)
target_compile_options(
  scry_project_options
  INTERFACE
    "$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Wall;-Wextra;-Wconversion;-Wshadow>"
)

# A tooling compiler older than the host's libstdc++ cannot parse its headers —
# clang 18 against Ubuntu 24.04's <expected>, for one — so the tooling build can
# select libc++ instead. No consumer build is affected: SCRY_CLANG_TOOLING is
# not one.
if(SCRY_CLANG_TOOLING_LIBCXX)
  target_compile_options(scry PUBLIC -stdlib=libc++)
  target_link_options(scry PUBLIC -stdlib=libc++)
  target_compile_options(scry_project_options INTERFACE -stdlib=libc++)
  target_link_options(scry_project_options INTERFACE -stdlib=libc++)
endif()

if(SCRY_CLANG_TOOLING AND NOT SCRY_TOOLING_HAS_STOP_TOKEN)
  target_compile_options(scry PUBLIC -fexperimental-library)
  target_link_options(scry PUBLIC -fexperimental-library)
  target_compile_options(scry_project_options INTERFACE -fexperimental-library)
  target_link_options(scry_project_options INTERFACE -fexperimental-library)
endif()
target_link_libraries(
  scry
  PRIVATE
    CURL::libcurl
    Threads::Threads
    "$<BUILD_INTERFACE:scry_project_options>"
)
add_subdirectory(src/machine)
add_subdirectory(src/protocol)
add_subdirectory(src/provider)
add_subdirectory(src/transport)

if(SCRY_WARNINGS_AS_ERRORS)
  target_compile_options(
    scry_project_options
    INTERFACE
      "$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror>"
  )
endif()

function(scry_enable_sanitizer FLAG)
  target_compile_options(
    scry_project_options
    INTERFACE "${FLAG}" -fno-omit-frame-pointer
  )
  target_link_options(
    scry_project_options
    INTERFACE "${FLAG}" -fno-omit-frame-pointer
  )
endfunction()

if(SCRY_SANITIZER STREQUAL "address-undefined")
  scry_enable_sanitizer("-fsanitize=address,undefined")
  target_compile_options(
    scry_project_options
    INTERFACE -fno-sanitize-recover=undefined
  )
elseif(SCRY_SANITIZER STREQUAL "thread")
  scry_enable_sanitizer("-fsanitize=thread")
elseif(NOT SCRY_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unsupported SCRY_SANITIZER: ${SCRY_SANITIZER}")
endif()

# libFuzzer can only steer through code carrying SanitizerCoverage, and targets
# like scry_conversation_fuzz link the whole library, so without this every
# mutation past the fuzz entry point would be blind. The "-no-link" spelling adds
# the instrumentation without libFuzzer's main, which ordinary test executables
# must not link; the fuzz targets add plain -fsanitize=fuzzer themselves.
if(SCRY_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(
    scry_project_options
    INTERFACE -fsanitize=fuzzer-no-link
  )
endif()

if(SCRY_ENABLE_CLANG_TIDY)
  find_program(SCRY_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
  set_property(
    TARGET scry
    PROPERTY
      CXX_CLANG_TIDY
        "${SCRY_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*"
  )
endif()

set(
  SCRY_CORE_PUBLIC_HEADERS
  include/scry/config.hpp
  include/scry/conversation.hpp
  include/scry/detail/reflection_codec.hpp
  include/scry/detail/reflection_json.hpp
  include/scry/detail/reflection_meta.hpp
  include/scry/detail/reflection_registration.hpp
  include/scry/detail/reflection_schema.hpp
  include/scry/error.hpp
  include/scry/events.hpp
  include/scry/harness.hpp
  include/scry/json.hpp
  include/scry/message.hpp
  include/scry/reflection.hpp
  include/scry/scry.hpp
  include/scry/tool_registry.hpp
  include/scry/turn.hpp
  include/scry/turn_id.hpp
  include/scry/unique_function.hpp
)
target_sources(
  scry
  PUBLIC
    FILE_SET scry_core_headers
    TYPE HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES ${SCRY_CORE_PUBLIC_HEADERS}
    FILE_SET scry_generated_headers
    TYPE HEADERS
    BASE_DIRS "${SCRY_GENERATED_INCLUDE_DIR}"
    FILES "${SCRY_GENERATED_VERSION_HEADER}"
)

if(NOT SCRY_CLANG_TOOLING)
  add_custom_target(
    scry_header_audit ALL
    COMMAND
      "${CMAKE_COMMAND}"
      "-DSCRY_PUBLIC_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
      "-DSCRY_GENERATED_INCLUDE_DIR=${SCRY_GENERATED_INCLUDE_DIR}"
      -P "${PROJECT_SOURCE_DIR}/cmake/CheckPublicHeaders.cmake"
    COMMENT "Auditing the public-header boundary"
    VERBATIM
  )
endif()

if(SCRY_ENABLE_FORMAT_CHECK)
  find_program(SCRY_CLANG_FORMAT_EXECUTABLE NAMES clang-format REQUIRED)
  file(
    GLOB_RECURSE
    SCRY_FORMAT_SOURCES
    CONFIGURE_DEPENDS
      "${PROJECT_SOURCE_DIR}/examples/*.cpp"
      "${PROJECT_SOURCE_DIR}/extras/*.cpp"
      "${PROJECT_SOURCE_DIR}/extras/*.hpp"
      "${PROJECT_SOURCE_DIR}/include/*.hpp"
      "${PROJECT_SOURCE_DIR}/src/*.cpp"
      "${PROJECT_SOURCE_DIR}/src/*.hpp"
      "${PROJECT_SOURCE_DIR}/tests/*.cpp"
      "${PROJECT_SOURCE_DIR}/tests/*.hpp"
  )

  add_custom_target(
    format-check
    COMMAND
      "${SCRY_CLANG_FORMAT_EXECUTABLE}"
      --dry-run
      --Werror
      ${SCRY_FORMAT_SOURCES}
    COMMENT "Checking C++ formatting"
    VERBATIM
  )
  add_custom_target(
    format
    COMMAND "${SCRY_CLANG_FORMAT_EXECUTABLE}" -i ${SCRY_FORMAT_SOURCES}
    COMMENT "Formatting C++ sources"
    VERBATIM
  )
endif()

if(SCRY_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(SCRY_BUILD_TESTS)
  # enable_testing() must run in the top-level directory for `ctest` to see the
  # suites registered under tests/, so CTest is included here rather than there.
  include(CTest)
  add_subdirectory(tests)
endif()

configure_package_config_file(
  cmake/scryConfig.cmake.in
  "${PROJECT_BINARY_DIR}/scryConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scry"
)
# Pre-1.0, minor releases may break API; SameMinorVersion keeps a consumer's
# find_package(scry 0.1) from silently accepting an incompatible 0.2 install.
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/scryConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMinorVersion
)

install(
  TARGETS scry
  EXPORT scryTargets
  FILE_SET scry_core_headers
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILE_SET scry_generated_headers
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
  EXPORT scryTargets
  FILE scryTargets.cmake
  NAMESPACE scry::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scry"
)
install(
  FILES
    "${PROJECT_BINARY_DIR}/scryConfig.cmake"
    "${PROJECT_BINARY_DIR}/scryConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scry"
)
install(
  FILES LICENSE
  DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
