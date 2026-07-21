cmake_minimum_required(VERSION 3.25)

project(
  scry
  VERSION 0.0.1
  DESCRIPTION "A main-loop-friendly C++ LLM harness"
  LANGUAGES CXX
)

# The public version header is the other place the release number lives;
# refuse to configure when the two disagree so they can never drift.
file(READ "${PROJECT_SOURCE_DIR}/include/scry/version.hpp" SCRY_VERSION_HEADER)
foreach(SCRY_VERSION_PART MAJOR MINOR PATCH)
  if(NOT SCRY_VERSION_HEADER MATCHES "#define SCRY_VERSION_${SCRY_VERSION_PART} ([0-9]+)")
    message(FATAL_ERROR "include/scry/version.hpp is missing SCRY_VERSION_${SCRY_VERSION_PART}")
  endif()
  set("SCRY_HEADER_VERSION_${SCRY_VERSION_PART}" "${CMAKE_MATCH_1}")
endforeach()
set(
  SCRY_HEADER_VERSION
  "${SCRY_HEADER_VERSION_MAJOR}.${SCRY_HEADER_VERSION_MINOR}.${SCRY_HEADER_VERSION_PATCH}"
)
if(NOT SCRY_HEADER_VERSION STREQUAL PROJECT_VERSION)
  message(
    FATAL_ERROR
    "include/scry/version.hpp reports ${SCRY_HEADER_VERSION} but the project "
    "version is ${PROJECT_VERSION}; bump both together"
  )
endif()

include(CMakePackageConfigHelpers)
include(FetchContent)
include(GNUInstallDirs)

option(SCRY_BUILD_TESTS "Build Scry's tests" ${PROJECT_IS_TOP_LEVEL})
option(SCRY_BUILD_EXAMPLES "Compile Scry's canonical examples" ${PROJECT_IS_TOP_LEVEL})
option(SCRY_ENABLE_FORMAT_CHECK "Create clang-format check and fix targets" OFF)
option(SCRY_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(SCRY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(SCRY_USE_LIBCXX "Use libc++ with a Clang-family compiler" OFF)
option(SCRY_ENABLE_REFLECTION "Build the supported GCC 16+ reflection component" OFF)
option(SCRY_BUILD_FUZZERS "Build short protocol fuzz targets" OFF)
option(
  SCRY_BUILD_IMGUI_SHOWCASE
  "Build the opt-in Dear ImGui and grid-NPC showcase"
  OFF
)
option(
  SCRY_BUILD_LOCAL_MODEL_SMOKE
  "Build the nightly public-API local-model smoke executable"
  OFF
)

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

find_package(CURL 7.84 REQUIRED)
find_package(Threads REQUIRED)

FetchContent_Declare(
  glaze
  GIT_REPOSITORY https://github.com/stephenberry/glaze.git
  GIT_TAG 8b60d82c66311c145c4d03be3b556b555a9cb111
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(glaze)

add_library(
  scry
  STATIC
    src/core/retry.cpp
    src/core/json_codec.cpp
    src/runtime/config.cpp
    src/runtime/conversation.cpp
    src/runtime/conversation_persistence.cpp
    src/runtime/harness.cpp
    src/runtime/messages.cpp
    src/runtime/pump.cpp
    src/runtime/pump_state.cpp
    src/runtime/queue.cpp
    src/runtime/state.cpp
    src/runtime/tool_registry.cpp
    src/runtime/tool_dispatch.cpp
    src/runtime/turn.cpp
    src/runtime/worker.cpp
    src/runtime/worker_publish.cpp
)
add_library(scry::scry ALIAS scry)
set_target_properties(scry PROPERTIES EXPORT_NAME scry)
target_compile_features(scry PUBLIC cxx_std_23)
target_include_directories(
  scry
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_include_directories(scry SYSTEM PRIVATE "${glaze_SOURCE_DIR}/include")

add_library(scry_project_options INTERFACE)
target_compile_options(
  scry_project_options
  INTERFACE
    "$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Wall;-Wextra;-Wconversion;-Wshadow>"
)
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

if(SCRY_USE_LIBCXX)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SCRY_USE_LIBCXX requires Clang or AppleClang")
  endif()
  target_compile_options(scry PUBLIC -stdlib=libc++)
  target_link_options(scry PUBLIC -stdlib=libc++)
  target_compile_options(scry_project_options INTERFACE -stdlib=libc++)
  target_link_options(scry_project_options INTERFACE -stdlib=libc++)
endif()

# LLVM 18 and Apple libc++ expose the standard C++20 stop-token/jthread
# surface only when their matching experimental-library mode is enabled.
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR SCRY_USE_LIBCXX)
  target_compile_options(scry PUBLIC -fexperimental-library)
  target_link_options(scry PUBLIC -fexperimental-library)
  target_compile_options(scry_project_options INTERFACE -fexperimental-library)
  target_link_options(scry_project_options INTERFACE -fexperimental-library)
endif()

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
elseif(SCRY_SANITIZER STREQUAL "thread")
  scry_enable_sanitizer("-fsanitize=thread")
elseif(NOT SCRY_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unsupported SCRY_SANITIZER: ${SCRY_SANITIZER}")
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
  include/scry/error.hpp
  include/scry/events.hpp
  include/scry/harness.hpp
  include/scry/json.hpp
  include/scry/scry.hpp
  include/scry/tool_registry.hpp
  include/scry/turn.hpp
  include/scry/turn_id.hpp
  include/scry/unique_function.hpp
  include/scry/version.hpp
)
target_sources(
  scry
  PUBLIC
    FILE_SET scry_core_headers
    TYPE HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES ${SCRY_CORE_PUBLIC_HEADERS}
)

add_custom_target(
  scry_header_audit ALL
  COMMAND
    "${CMAKE_COMMAND}"
    "-DSCRY_PUBLIC_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
    -P "${PROJECT_SOURCE_DIR}/cmake/CheckPublicHeaders.cmake"
  COMMENT "Auditing the public-header boundary"
  VERBATIM
)

if(SCRY_ENABLE_FORMAT_CHECK)
  find_program(SCRY_CLANG_FORMAT_EXECUTABLE NAMES clang-format REQUIRED)
  file(
    GLOB_RECURSE
    SCRY_FORMAT_SOURCES
    CONFIGURE_DEPENDS
      "${PROJECT_SOURCE_DIR}/examples/*.cpp"
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

if(SCRY_ENABLE_REFLECTION)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "Scry reflection is supported only with GCC 16 or newer")
  endif()
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "Scry reflection requires GCC 16 or newer")
  endif()

  include(CheckCXXSourceCompiles)
  set(SCRY_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "-std=c++26 -freflection")
  unset(SCRY_COMPILER_HAS_P2996 CACHE)
  check_cxx_source_compiles(
    "
      #include <meta>
      #if !defined(__cpp_impl_reflection)
      #error P2996 feature macro is missing
      #endif
      int main() { return 0; }
    "
    SCRY_COMPILER_HAS_P2996
  )
  set(CMAKE_REQUIRED_FLAGS "${SCRY_SAVED_REQUIRED_FLAGS}")
  if(NOT SCRY_COMPILER_HAS_P2996)
    message(FATAL_ERROR "The selected compiler cannot compile the P2996 surface")
  endif()

  file(
    GLOB_RECURSE
    SCRY_REFLECTION_DETAIL_HEADERS
    CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/scry/detail/reflection*.hpp"
    "${PROJECT_SOURCE_DIR}/include/scry/detail/reflection/*.hpp"
  )
  set(
    SCRY_REFLECTION_PUBLIC_HEADERS
    include/scry/reflection.hpp
    ${SCRY_REFLECTION_DETAIL_HEADERS}
  )

  add_library(scry_reflection STATIC src/reflection/json_bridge.cpp)
  add_library(scry::reflection ALIAS scry_reflection)
  set_target_properties(
    scry_reflection
    PROPERTIES
      CXX_EXTENSIONS OFF
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      EXPORT_NAME reflection
  )
  target_sources(
    scry_reflection
    PUBLIC
      FILE_SET scry_reflection_headers
      TYPE HEADERS
      BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
      FILES ${SCRY_REFLECTION_PUBLIC_HEADERS}
  )
  target_compile_features(scry_reflection PUBLIC cxx_std_26)
  target_compile_definitions(scry_reflection PUBLIC SCRY_ENABLE_REFLECTION=1)
  target_compile_options(
    scry_reflection
    PUBLIC -std=c++26 -freflection
  )
  target_include_directories(
    scry_reflection
    PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
  )
  target_include_directories(
    scry_reflection
    SYSTEM PRIVATE "${glaze_SOURCE_DIR}/include"
  )
  target_link_libraries(
    scry_reflection
    PUBLIC scry::scry
    PRIVATE "$<BUILD_INTERFACE:scry_project_options>"
  )

endif()

if(SCRY_BUILD_EXAMPLES OR SCRY_BUILD_IMGUI_SHOWCASE)
  add_subdirectory(examples)
endif()

if(SCRY_BUILD_LOCAL_MODEL_SMOKE)
  add_executable(
    scry_local_model_smoke
    tests/nightly/local_model_smoke.cpp
  )
  target_link_libraries(
    scry_local_model_smoke
    PRIVATE scry::scry scry_project_options
  )
endif()

if(SCRY_BUILD_TESTS)
  include(CTest)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG dfc2dff8d70d083c60c1c6986030e5389a867a93
    SYSTEM
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(Catch2)
  foreach(SCRY_CATCH_TARGET IN ITEMS Catch2 Catch2WithMain)
    target_compile_features("${SCRY_CATCH_TARGET}" PRIVATE cxx_std_23)
    if(SCRY_USE_LIBCXX)
      target_compile_options("${SCRY_CATCH_TARGET}" PRIVATE -stdlib=libc++)
    endif()
  endforeach()
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  include(Catch)

  file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated/header_checks")
  foreach(SCRY_HEADER IN LISTS SCRY_CORE_PUBLIC_HEADERS)
    get_filename_component(SCRY_HEADER_NAME "${SCRY_HEADER}" NAME_WE)
    string(REGEX REPLACE "^include/" "" SCRY_HEADER_INCLUDE "${SCRY_HEADER}")
    set(
      SCRY_HEADER_CHECK_SOURCE
      "${PROJECT_BINARY_DIR}/generated/header_checks/${SCRY_HEADER_NAME}.cpp"
    )
    file(
      GENERATE
      OUTPUT "${SCRY_HEADER_CHECK_SOURCE}"
      CONTENT "#include <${SCRY_HEADER_INCLUDE}>\n"
    )
    add_library(
      "scry_header_${SCRY_HEADER_NAME}"
      OBJECT
      "${SCRY_HEADER_CHECK_SOURCE}"
    )
    target_link_libraries(
      "scry_header_${SCRY_HEADER_NAME}"
      PRIVATE scry::scry scry_project_options
    )
  endforeach()
  if(SCRY_ENABLE_REFLECTION)
    set(
      SCRY_REFLECTION_HEADER_CHECK_SOURCE
      "${PROJECT_BINARY_DIR}/generated/header_checks/reflection.cpp"
    )
    file(
      GENERATE
      OUTPUT "${SCRY_REFLECTION_HEADER_CHECK_SOURCE}"
      CONTENT "#include <scry/reflection.hpp>\n"
    )
    add_library(
      scry_header_reflection
      OBJECT
      "${SCRY_REFLECTION_HEADER_CHECK_SOURCE}"
    )
    target_link_libraries(
      scry_header_reflection
      PRIVATE scry::reflection scry_project_options
    )
  endif()

  add_executable(scry_public_api_contract tests/public_api_contract.cpp)
  target_link_libraries(
    scry_public_api_contract
    PRIVATE scry::scry scry_project_options
  )
  add_test(NAME public-api-contract COMMAND scry_public_api_contract)

  add_executable(
    scry_retry_tests
    tests/core/retry_tests.cpp
    src/core/retry.cpp
  )
  target_compile_features(scry_retry_tests PRIVATE cxx_std_23)
  target_include_directories(
    scry_retry_tests
    PRIVATE
      "${PROJECT_SOURCE_DIR}/include"
      "${PROJECT_SOURCE_DIR}/src"
  )
  target_link_libraries(
    scry_retry_tests
    PRIVATE Catch2::Catch2WithMain scry_project_options
  )
  catch_discover_tests(scry_retry_tests TEST_PREFIX "core.")

  add_executable(
    scry_runtime_tests
    tests/runtime/config_tests.cpp
    tests/runtime/conversation_persistence_tests.cpp
    tests/runtime/pump_branch_tests.cpp
    tests/runtime/pump_regression_tests.cpp
    tests/runtime/queue_coverage_tests.cpp
    tests/runtime/runtime_coverage_tests.cpp
    tests/runtime/runtime_tests.cpp
    tests/runtime/tool_dispatch_tests.cpp
    tests/runtime/tool_pump_tests.cpp
    tests/runtime/tool_registry_tests.cpp
  )
  target_include_directories(scry_runtime_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_include_directories(
    scry_runtime_tests
    SYSTEM PRIVATE "${glaze_SOURCE_DIR}/include"
  )
  target_link_libraries(
    scry_runtime_tests
    PRIVATE
      Catch2::Catch2WithMain
      glaze::glaze
      scry::scry
      scry_project_options
  )
  catch_discover_tests(scry_runtime_tests TEST_PREFIX "runtime.")
  add_subdirectory(tests/machine)
  add_subdirectory(tests/protocol)
  add_subdirectory(tests/provider)
  add_subdirectory(tests/transport)
  add_subdirectory(tests/integration)
  if(SCRY_BUILD_IMGUI_SHOWCASE)
    add_subdirectory(tests/showcase)
  endif()

  if(SCRY_ENABLE_REFLECTION)
    file(
      GLOB
      SCRY_REFLECTION_TEST_SOURCES
      CONFIGURE_DEPENDS
      "${PROJECT_SOURCE_DIR}/tests/reflection/*.cpp"
    )
    if(SCRY_REFLECTION_TEST_SOURCES)
      add_executable(
        scry_reflection_tests
        ${SCRY_REFLECTION_TEST_SOURCES}
      )
      target_link_libraries(
        scry_reflection_tests
        PRIVATE
          Catch2::Catch2WithMain
          scry::reflection
          scry_project_options
      )
      catch_discover_tests(
        scry_reflection_tests
        TEST_PREFIX "reflection."
        PROPERTIES LABELS reflection
      )
    endif()

    function(
      scry_add_reflection_compile_fail_test
      TEST_NAME
      SOURCE_NAME
      EXPECTED_SUBSTRING
    )
      set(
        SCRY_COMPILE_FAIL_TEST_NAME
        "reflection.compile-fail.${TEST_NAME}"
      )
      add_test(
        NAME "${SCRY_COMPILE_FAIL_TEST_NAME}"
        COMMAND
          "${CMAKE_COMMAND}"
          "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
          "-DSOURCE_FILE=${PROJECT_SOURCE_DIR}/tests/reflection/compile_fail/${SOURCE_NAME}"
          "-DPROJECT_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
          "-DEXPECTED_SUBSTRING=${EXPECTED_SUBSTRING}"
          "-DOUTPUT_DIRECTORY=${PROJECT_BINARY_DIR}/reflection-compile-fail"
          -P "${PROJECT_SOURCE_DIR}/cmake/ExpectCompileFailure.cmake"
      )
      set_tests_properties(
        "${SCRY_COMPILE_FAIL_TEST_NAME}"
        PROPERTIES LABELS reflection
      )
    endfunction()

    scry_add_reflection_compile_fail_test(
      unsupported-arguments
      unsupported_arguments.cpp
      "ToolArguments"
    )
    scry_add_reflection_compile_fail_test(
      void-handler
      void_handler.cpp
      "ToolHandlerFor"
    )
    scry_add_reflection_compile_fail_test(
      raw-json-handler
      raw_json_handler.cpp
      "ToolHandlerFor"
    )
    scry_add_reflection_compile_fail_test(
      unknown-description
      unknown_description.cpp
      "naming distinct reflected members"
    )
    scry_add_reflection_compile_fail_test(
      duplicate-description
      duplicate_description.cpp
      "naming distinct reflected members"
    )
  endif()
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
)
if(TARGET scry_reflection)
  install(
    TARGETS scry_reflection
    EXPORT scryReflectionTargets
    FILE_SET scry_reflection_headers
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )
  install(
    EXPORT scryReflectionTargets
    FILE scryReflectionTargets.cmake
    NAMESPACE scry::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scry"
  )
endif()
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
  FILES LICENSE THIRD_PARTY_NOTICES.md
  DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
