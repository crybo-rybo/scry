cmake_minimum_required(VERSION 3.25)

project(
  scry
  VERSION 0.0.0
  DESCRIPTION "A main-loop-friendly C++ LLM harness"
  LANGUAGES CXX
)

include(CMakePackageConfigHelpers)
include(FetchContent)
include(GNUInstallDirs)

option(SCRY_BUILD_TESTS "Build Scry's tests" ${PROJECT_IS_TOP_LEVEL})
option(SCRY_BUILD_EXAMPLES "Compile Scry's canonical examples" ${PROJECT_IS_TOP_LEVEL})
option(SCRY_ENABLE_FORMAT_CHECK "Create clang-format check and fix targets" OFF)
option(SCRY_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(SCRY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(SCRY_USE_LIBCXX "Use libc++ with a Clang-family compiler" OFF)
option(SCRY_ENABLE_REFLECTION "Enable the experimental C++26 reflection surface" OFF)
option(SCRY_BUILD_REFLECTION_SPIKE "Build the Glaze/P2996 feasibility spike" OFF)
option(SCRY_BUILD_CURL_SPIKE "Build the libcurl/SSE feasibility spike" OFF)

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

if(SCRY_BUILD_REFLECTION_SPIKE)
  set(glaze_ENABLE_REFLECTION26 ON CACHE BOOL "" FORCE)
endif()
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
    src/runtime/conversation.cpp
    src/runtime/messages.cpp
    src/runtime/pump.cpp
    src/runtime/queue.cpp
    src/runtime/state.cpp
    src/runtime/tool_registry.cpp
    src/runtime/turn.cpp
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

if(SCRY_USE_LIBCXX)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SCRY_USE_LIBCXX requires Clang or AppleClang")
  endif()
  target_compile_options(scry PUBLIC -stdlib=libc++)
  target_link_options(scry PUBLIC -stdlib=libc++)
  target_compile_options(scry_project_options INTERFACE -stdlib=libc++)
  target_link_options(scry_project_options INTERFACE -stdlib=libc++)
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
  set(
    CMAKE_CXX_CLANG_TIDY
    "${SCRY_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*"
  )
endif()

set(
  SCRY_PUBLIC_HEADERS
  include/scry/config.hpp
  include/scry/conversation.hpp
  include/scry/error.hpp
  include/scry/events.hpp
  include/scry/harness.hpp
  include/scry/json.hpp
  include/scry/reflection.hpp
  include/scry/scry.hpp
  include/scry/tool_registry.hpp
  include/scry/turn.hpp
  include/scry/turn_id.hpp
  include/scry/unique_function.hpp
  include/scry/version.hpp
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
      "${PROJECT_SOURCE_DIR}/spikes/*.cpp"
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
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
      message(FATAL_ERROR "Scry reflection requires GCC 16 or newer")
    endif()
    set(SCRY_REFLECTION_COMPILE_OPTIONS -freflection)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(
      WARNING
        "clang-p2996 is a non-production compatibility target; do not ship its artifacts"
    )
    set(
      SCRY_REFLECTION_COMPILE_OPTIONS
      -freflection
      -fexpansion-statements
      -stdlib=libc++
    )
  else()
    message(FATAL_ERROR "No supported C++26 reflection toolchain was detected")
  endif()

  include(CheckCXXSourceCompiles)
  string(
    JOIN " "
    SCRY_REFLECTION_REQUIRED_FLAGS
    -std=c++26
    ${SCRY_REFLECTION_COMPILE_OPTIONS}
  )
  set(SCRY_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "${SCRY_REFLECTION_REQUIRED_FLAGS}")
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

  add_library(scry_reflection INTERFACE)
  add_library(scry::reflection ALIAS scry_reflection)
  set_target_properties(scry_reflection PROPERTIES EXPORT_NAME reflection)
  target_link_libraries(scry_reflection INTERFACE scry)
  target_compile_features(scry_reflection INTERFACE cxx_std_26)
  target_compile_definitions(scry_reflection INTERFACE SCRY_ENABLE_REFLECTION=1)
  target_compile_options(
    scry_reflection
    INTERFACE ${SCRY_REFLECTION_COMPILE_OPTIONS}
  )
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_options(scry_reflection INTERFACE -stdlib=libc++)
  endif()
endif()

if(SCRY_BUILD_REFLECTION_SPIKE)
  if(NOT SCRY_ENABLE_REFLECTION)
    message(
      FATAL_ERROR
        "SCRY_BUILD_REFLECTION_SPIKE requires SCRY_ENABLE_REFLECTION=ON"
    )
  endif()

  add_executable(scry_reflection_spike spikes/reflection_glaze.cpp)
  target_link_libraries(
    scry_reflection_spike
    PRIVATE scry::reflection glaze::glaze scry_project_options
  )
endif()

if(SCRY_BUILD_EXAMPLES)
  add_library(scry_canonical_example OBJECT examples/main_loop.cpp)
  target_link_libraries(
    scry_canonical_example
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
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  include(Catch)

  file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated/header_checks")
  foreach(SCRY_HEADER IN LISTS SCRY_PUBLIC_HEADERS)
    if(SCRY_HEADER MATCHES "/reflection.hpp$")
      continue()
    endif()
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

  add_executable(scry_public_api_contract tests/public_api_contract.cpp)
  target_link_libraries(
    scry_public_api_contract
    PRIVATE scry::scry scry_project_options
  )
  add_test(NAME public-api-contract COMMAND scry_public_api_contract)

  add_executable(scry_retry_tests tests/core/retry_tests.cpp)
  target_include_directories(scry_retry_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(
    scry_retry_tests
    PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
  )
  catch_discover_tests(scry_retry_tests TEST_PREFIX "core.")

  add_executable(scry_runtime_tests tests/runtime/runtime_tests.cpp)
  target_include_directories(scry_runtime_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(
    scry_runtime_tests
    PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
  )
  catch_discover_tests(scry_runtime_tests TEST_PREFIX "runtime.")
endif()

if(SCRY_BUILD_CURL_SPIKE)
  find_package(CURL 7.84 REQUIRED)
  add_executable(scry_curl_sse_spike spikes/curl_sse.cpp)
  target_compile_features(scry_curl_sse_spike PRIVATE cxx_std_23)
  target_link_libraries(
    scry_curl_sse_spike
    PRIVATE CURL::libcurl scry_project_options
  )
  if(SCRY_BUILD_TESTS)
    add_test(NAME curl-sse-feasibility COMMAND scry_curl_sse_spike)
  endif()
endif()

configure_package_config_file(
  cmake/scryConfig.cmake.in
  "${PROJECT_BINARY_DIR}/scryConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scry"
)
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/scryConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)

install(
  TARGETS scry
  EXPORT scryTargets
)
if(TARGET scry_reflection)
  install(
    TARGETS scry_reflection
    EXPORT scryTargets
  )
endif()
install(
  DIRECTORY include/
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
