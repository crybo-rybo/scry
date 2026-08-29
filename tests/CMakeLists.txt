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

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
  NAME performance-tooling
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/perf_compare_tests.py"
)

# Every public header must compile on its own, so each one becomes a lone
# translation unit that includes nothing else.
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

add_executable(scry_public_api_contract public_api_contract.cpp)
target_link_libraries(
  scry_public_api_contract
  PRIVATE scry::scry scry_project_options
)
add_test(NAME public-api-contract COMMAND scry_public_api_contract)

add_executable(
  scry_runtime_tests
  runtime/config_tests.cpp
  runtime/conversation_persistence_tests.cpp
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

add_subdirectory(machine)
if(SCRY_ENABLE_LOGGING)
  add_subdirectory(logging)
endif()
add_subdirectory(protocol)
add_subdirectory(provider)
add_subdirectory(transport)
add_subdirectory(integration)
if(SCRY_BUILD_IMGUI_SHOWCASE)
  add_subdirectory(showcase)
endif()

if(SCRY_ENABLE_REFLECTION)
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
        scry::reflection
        scry_project_options
    )
    target_include_directories(scry_reflection_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
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
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/reflection/compile_fail/${SOURCE_NAME}"
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
    duplicate-description
    duplicate_description.cpp
    "a reflected member may have at most one scry::reflection::description annotation"
  )
endif()
