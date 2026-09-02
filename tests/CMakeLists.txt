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
endforeach()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# Every public header must compile on its own, so each one becomes a lone
# translation unit that includes nothing else. The reflected headers are part of
# that set, so this cannot run under SCRY_CLANG_TOOLING.
if(NOT SCRY_CLANG_TOOLING)
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

if(SCRY_BUILD_FUZZERS)
  scry_require_fuzzer_compiler()
  set(SCRY_FUZZ_TEST_PREFIX "runtime.")
  # The document codec pulls in the pump and worker through Conversation's
  # translation units, so this target links the library rather than curating a
  # minimal source set that would drift with every runtime change.
  scry_add_fuzzer(
    scry_conversation_fuzz
    conversation
    SOURCES runtime/conversation_fuzz.cpp
    LINK_LIBRARIES scry::scry
  )
endif()

add_subdirectory(machine)
add_subdirectory(protocol)
add_subdirectory(provider)
add_subdirectory(transport)
add_subdirectory(integration)
if(SCRY_BUILD_IMGUI_SHOWCASE)
  add_subdirectory(showcase)
endif()

# The reflected surface is the public API; it cannot compile under Clang, so
# the fuzz/tidy tooling build skips it.
if(NOT SCRY_CLANG_TOOLING)
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
    target_include_directories(scry_reflection_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
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
    duplicate-description
    duplicate_description.cpp
    "a reflected member may have at most one scry::reflection::description annotation"
  )
endif()
