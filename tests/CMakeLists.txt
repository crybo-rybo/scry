FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG 317ac1ed4c0bb6e6b91eafc817e05c488feffcb3
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Catch2)
foreach(SCRY_CATCH_TARGET IN ITEMS Catch2 Catch2WithMain)
  target_compile_features("${SCRY_CATCH_TARGET}" PRIVATE cxx_std_23)
endforeach()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# Every public header must compile on its own, so each one becomes a lone
# translation unit that includes nothing else.
# The generated <scry/version.hpp> lives in the build tree, so the loop derives
# the include spelling from the last "include/" in each path rather than
# assuming a source-relative one.
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated/header_checks")
add_library(scry_header_checks OBJECT)
# Keep one translation unit per header even in a unity build.
set_target_properties(scry_header_checks PROPERTIES UNITY_BUILD OFF)
target_link_libraries(scry_header_checks PRIVATE scry::scry scry_project_options)
foreach(
  SCRY_HEADER
  IN
  LISTS SCRY_CORE_PUBLIC_HEADERS
  ITEMS "${SCRY_GENERATED_VERSION_HEADER}"
)
  get_filename_component(SCRY_HEADER_NAME "${SCRY_HEADER}" NAME_WE)
  string(REGEX REPLACE "^.*include/" "" SCRY_HEADER_INCLUDE "${SCRY_HEADER}")
  set(
    SCRY_HEADER_CHECK_SOURCE
    "${PROJECT_BINARY_DIR}/generated/header_checks/${SCRY_HEADER_NAME}.cpp"
  )
  file(
    GENERATE
    OUTPUT "${SCRY_HEADER_CHECK_SOURCE}"
    CONTENT "#include <${SCRY_HEADER_INCLUDE}>\n"
  )
  target_sources(scry_header_checks PRIVATE "${SCRY_HEADER_CHECK_SOURCE}")
endforeach()

if(SCRY_BUILD_TESTING_SUPPORT)
  # The testing component's headers get the same lone-translation-unit check,
  # on a target of their own because they link scry::testing.
  add_library(scry_testing_header_checks OBJECT)
  set_target_properties(scry_testing_header_checks PROPERTIES UNITY_BUILD OFF)
  target_link_libraries(
    scry_testing_header_checks
    PRIVATE scry::testing scry_project_options
  )
  foreach(SCRY_HEADER IN LISTS SCRY_TESTING_PUBLIC_HEADERS)
    get_filename_component(SCRY_HEADER_NAME "${SCRY_HEADER}" NAME_WE)
    string(REGEX REPLACE "^.*include/" "" SCRY_HEADER_INCLUDE "${SCRY_HEADER}")
    set(
      SCRY_HEADER_CHECK_SOURCE
      "${PROJECT_BINARY_DIR}/generated/header_checks/testing_${SCRY_HEADER_NAME}.cpp"
    )
    file(
      GENERATE
      OUTPUT "${SCRY_HEADER_CHECK_SOURCE}"
      CONTENT "#include <${SCRY_HEADER_INCLUDE}>\n"
    )
    target_sources(scry_testing_header_checks PRIVATE "${SCRY_HEADER_CHECK_SOURCE}")
  endforeach()
endif()

add_executable(scry_public_api_contract public_api_contract.cpp)
target_link_libraries(
  scry_public_api_contract
  PRIVATE scry::scry scry_project_options
)
add_test(NAME public-api-contract COMMAND scry_public_api_contract)

if(SCRY_SANITIZER STREQUAL "address-undefined")
  add_executable(
    scry_ubsan_fatal_probe
    sanitizer/ubsan_fatal_probe.cpp
  )
  target_link_libraries(scry_ubsan_fatal_probe PRIVATE scry_project_options)
  add_test(
    NAME sanitizer.ubsan-is-fatal
    COMMAND
      "${CMAKE_COMMAND}"
      "-DPROBE=$<TARGET_FILE:scry_ubsan_fatal_probe>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/sanitizer/verify_ubsan_fatal.cmake"
  )
endif()

add_executable(
  scry_runtime_tests
  runtime/config_tests.cpp
  runtime/conversation_persistence_tests.cpp
  runtime/json_view_tests.cpp
  runtime/pump_regression_tests.cpp
  runtime/queue_tests.cpp
  runtime/runtime_tests.cpp
  runtime/tool_dispatch_tests.cpp
  runtime/tool_pump_tests.cpp
  runtime/tool_registry_tests.cpp
)
target_include_directories(
  scry_runtime_tests PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests"
)
target_include_directories(
  scry_runtime_tests
  SYSTEM PRIVATE "${SCRY_GLAZE_INCLUDE_DIR}"
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

if(SCRY_BUILD_TESTING_SUPPORT)
  add_subdirectory(testing)
endif()
add_subdirectory(machine)
add_subdirectory(nightly)
add_subdirectory(protocol)
add_subdirectory(provider)
add_subdirectory(transport)
add_subdirectory(integration)

file(
  GLOB
  SCRY_REFLECTION_TEST_SOURCES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/reflection/*.cpp"
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
      scry::scry
      scry_project_options
  )
  target_include_directories(
    scry_reflection_tests PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests"
  )
  catch_discover_tests(scry_reflection_tests TEST_PREFIX "reflection.")
endif()

function(
  scry_add_reflection_compile_fail_test
  TEST_NAME
  SOURCE_NAME
  EXPECTED_SUBSTRING
)
  add_test(
    NAME "reflection.compile-fail.${TEST_NAME}"
    COMMAND
      "${CMAKE_COMMAND}"
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/reflection/compile_fail/${SOURCE_NAME}"
      "-DPROJECT_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
      "-DEXPECTED_SUBSTRING=${EXPECTED_SUBSTRING}"
      "-DOUTPUT_DIRECTORY=${PROJECT_BINARY_DIR}/reflection-compile-fail"
      -P "${PROJECT_SOURCE_DIR}/cmake/ExpectCompileFailure.cmake"
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
  wrong-context-order
  wrong_context_order.cpp
  "ToolHandlerFor"
)
scry_add_reflection_compile_fail_test(
  duplicate-description
  duplicate_description.cpp
  "a reflected member may have at most one scry::reflection::description annotation"
)
scry_add_reflection_compile_fail_test(
  unsupported-schema-root
  unsupported_schema_root.cpp
  "SupportedValue"
)
