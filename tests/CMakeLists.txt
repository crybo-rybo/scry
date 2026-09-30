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
# translation unit that includes nothing else, on an object target of its own
# per library. The generated <scry/version.hpp> lives in the build tree, so the
# include spelling is derived from the last "include/" in each path.
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated/header_checks")
function(scry_add_header_checks TARGET LIBRARY PREFIX)
  add_library("${TARGET}" OBJECT)
  # Keep one translation unit per header even in a unity build.
  set_target_properties("${TARGET}" PROPERTIES UNITY_BUILD OFF)
  target_link_libraries("${TARGET}" PRIVATE "${LIBRARY}" scry_project_options)
  foreach(SCRY_HEADER IN LISTS ARGN)
    get_filename_component(SCRY_HEADER_NAME "${SCRY_HEADER}" NAME_WE)
    string(REGEX REPLACE "^.*include/" "" SCRY_HEADER_INCLUDE "${SCRY_HEADER}")
    set(
      SCRY_HEADER_CHECK_SOURCE
      "${PROJECT_BINARY_DIR}/generated/header_checks/${PREFIX}${SCRY_HEADER_NAME}.cpp"
    )
    file(
      GENERATE
      OUTPUT "${SCRY_HEADER_CHECK_SOURCE}"
      CONTENT "#include <${SCRY_HEADER_INCLUDE}>\n"
    )
    target_sources("${TARGET}" PRIVATE "${SCRY_HEADER_CHECK_SOURCE}")
  endforeach()
endfunction()

scry_add_header_checks(
  scry_header_checks scry::scry ""
  ${SCRY_CORE_PUBLIC_HEADERS} "${SCRY_GENERATED_VERSION_HEADER}"
)
if(SCRY_BUILD_TESTING_SUPPORT)
  scry_add_header_checks(
    scry_testing_header_checks scry::testing "testing_"
    ${SCRY_TESTING_PUBLIC_HEADERS}
  )
endif()

add_executable(scry_public_api_contract public_api_contract.cpp)
target_link_libraries(
  scry_public_api_contract
  PRIVATE scry::scry scry_project_options
)
add_test(NAME public-api-contract COMMAND scry_public_api_contract)

# The kernel's include boundary: allowlisted public headers only, no non-kernel
# src/ header, and the reflection guard in every file (src/kernel/kernel.hpp).
add_test(
  NAME kernel.include-boundary
  COMMAND
    "${CMAKE_COMMAND}"
    "-DSCRY_SOURCE_DIR=${PROJECT_SOURCE_DIR}/src"
    -P "${PROJECT_SOURCE_DIR}/cmake/CheckKernelBoundary.cmake"
)

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
  runtime/answer_pump_tests.cpp
  runtime/codec_internals_tests.cpp
  runtime/config_tests.cpp
  runtime/conversation_persistence_tests.cpp
  runtime/json_codec_validation_tests.cpp
  runtime/json_view_tests.cpp
  runtime/message_reflection_tests.cpp
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
# runtime/json_codec_validation_tests.cpp holds the codec entry points and
# JsonView to the JSON golden fixtures, and replays every prefix of every
# checked-in fuzz corpus through them.
target_compile_definitions(
  scry_runtime_tests
  PRIVATE
    "SCRY_FUZZ_CORPUS_DIR=\"${PROJECT_SOURCE_DIR}/tests/fuzz/corpus\""
    "SCRY_JSON_FIXTURE_DIR=\"${PROJECT_SOURCE_DIR}/tests/fixtures/json\""
)
target_link_libraries(
  scry_runtime_tests
  PRIVATE
    Catch2::Catch2WithMain
    scry::scry
    scry_project_options
)
catch_discover_tests(scry_runtime_tests TEST_PREFIX "runtime.")

if(SCRY_BUILD_TESTING_SUPPORT)
  add_subdirectory(testing)
endif()
add_subdirectory(kernel)
add_subdirectory(machine)
add_subdirectory(e2e)
add_subdirectory(protocol)
add_subdirectory(provider)
add_subdirectory(transport)
add_subdirectory(integration)

# The fuzz targets over the reflective side of the library link scry, so they
# cannot be libFuzzer binaries; this build replays their seed corpora instead
# (cmake/ScryFuzz.cmake). The kernel targets are libFuzzer binaries in the Clang
# tooling build (tests/fuzz/CMakeLists.txt).
include(ScryFuzz)
scry_add_fuzz_replay(
  scry_anthropic_fuzz_replay
  anthropic
  TEST_PREFIX "provider."
  SOURCES provider/anthropic_fuzz.cpp
  LINK_LIBRARIES scry::scry
)
scry_add_fuzz_replay(
  scry_openai_fuzz_replay
  openai
  TEST_PREFIX "provider."
  SOURCES provider/openai_fuzz.cpp
  LINK_LIBRARIES scry::scry
)
# The JSON layer's property target is a kernel libFuzzer target too; replaying
# its corpus here as well runs it under GCC and the GCC sanitizer presets.
scry_add_fuzz_replay(
  scry_json_fuzz_replay
  json
  TEST_PREFIX "kernel."
  SOURCES fuzz/json_fuzz.cpp
  LINK_LIBRARIES scry_kernel
)
scry_add_fuzz_replay(
  scry_conversation_fuzz_replay
  conversation
  TEST_PREFIX "runtime."
  SOURCES runtime/conversation_fuzz.cpp
  LINK_LIBRARIES scry::scry
)

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

# A registration must generate code from the declarations its own translation
# unit sees (a namespace's tools, a function's parameter names and annotations, a
# toolbox member's annotations), however the linker orders the units, so the same
# program is linked in both orders.
foreach(SCRY_LINK_ORDER IN ITEMS a_then_b b_then_a)
  if(SCRY_LINK_ORDER STREQUAL "a_then_b")
    set(SCRY_MULTI_TU_UNITS tools_a.cpp tools_b.cpp)
  else()
    set(SCRY_MULTI_TU_UNITS tools_b.cpp tools_a.cpp)
  endif()
  list(TRANSFORM SCRY_MULTI_TU_UNITS PREPEND "reflection/multi_tu/")
  add_executable(
    scry_multi_tu_${SCRY_LINK_ORDER}
    reflection/multi_tu/main.cpp
    ${SCRY_MULTI_TU_UNITS}
  )
  target_link_libraries(
    scry_multi_tu_${SCRY_LINK_ORDER}
    PRIVATE scry::scry scry_project_options
  )
  add_test(
    NAME "reflection.multi-tu-${SCRY_LINK_ORDER}"
    COMMAND scry_multi_tu_${SCRY_LINK_ORDER}
  )
endforeach()

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
      -P "${PROJECT_SOURCE_DIR}/cmake/ExpectCompileFailure.cmake"
  )
endfunction()

scry_add_reflection_compile_fail_test(
  unsupported-arguments
  unsupported_arguments.cpp
  "does not satisfy scry::reflection::ToolArguments: UnsupportedArguments::value: char is a character type"
)
scry_add_reflection_compile_fail_test(
  wrong-context-order
  wrong_context_order.cpp
  "is not invocable as (Arguments) or as (const scry::ToolCallContext&, Arguments)"
)
scry_add_reflection_compile_fail_test(
  duplicate-description
  duplicate_description.cpp
  "Arguments::value: a reflected member may have at most one scry::reflection::description annotation"
)
scry_add_reflection_compile_fail_test(
  unsupported-schema-root
  unsupported_schema_root.cpp
  "does not satisfy scry::reflection::SupportedValue: scry::Json is raw JSON, which has no schema"
)
scry_add_reflection_compile_fail_test(
  untagged-variant
  untagged_variant.cpp
  "Arguments::shape: std::variant<Circle, Square> has the alternative Circle without a scry::reflection::tag annotation"
)
scry_add_reflection_compile_fail_test(
  duplicate-tag
  duplicate_tag.cpp
  "has alternatives Circle and Square that share the tag"
)
scry_add_reflection_compile_fail_test(
  duplicate-key
  duplicate_key.cpp
  "does not satisfy scry::reflection::Encodable: Limits has members limit and max_tokens that both map to the JSON key"
)
scry_add_reflection_compile_fail_test(
  tag-key-collision
  tag_key_collision.cpp
  "does not satisfy scry::reflection::Decodable: Event::type: has the JSON key"
)
# The type after the path is the dealiased std::chrono::seconds, whose spelling
# varies with the platform's int64_t, so the assertion stops at the path.
scry_add_reflection_compile_fail_test(
  unsupported-nested-member
  unsupported_nested_member.cpp
  "does not satisfy scry::reflection::ToolArguments: ForecastArgs::window.when: std::chrono::duration<"
)

# Reflected tool declarations: each rejection names the function, member, or
# namespace at fault and the reason.
scry_add_reflection_compile_fail_test(
  toolbox-without-tools
  toolbox_without_tools.cpp
  "Lamp does not satisfy scry::reflection::Toolbox: Lamp declares no function annotated with scry::reflection::tool"
)
scry_add_reflection_compile_fail_test(
  tool-on-non-function
  tool_on_non_function.cpp
  "scry::ToolRegistry::add<^^lamp_tools>(): lamp_tools::brightness carries scry::reflection::tool, which applies to functions"
)
scry_add_reflection_compile_fail_test(
  tool-on-data-member
  tool_on_data_member.cpp
  "Arguments::city: carries scry::reflection::tool, which applies to functions"
)
scry_add_reflection_compile_fail_test(
  namespace-without-tools
  namespace_without_tools.cpp
  "scry::ToolRegistry::add<^^lamp_tools>(): namespace lamp_tools declares no function annotated with scry::reflection::tool"
)
scry_add_reflection_compile_fail_test(
  unnamed-parameter
  unnamed_parameter.cpp
  "lamp_tools::dim has the unnamed parameter 2; an argument synthesized from a parameter is named after it"
)
scry_add_reflection_compile_fail_test(
  unsupported-parameter
  unsupported_parameter.cpp
  "lamp_tools::label has parameter `initial`: char is a character type"
)
scry_add_reflection_compile_fail_test(
  mutable-reference-parameter
  mutable_reference_parameter.cpp
  "lamp_tools::rename has parameter `name` of type"
)
scry_add_reflection_compile_fail_test(
  unsupported-tool-result
  unsupported_tool_result.cpp
  "Lamp::describe returns scry::Json, which is not void, scry::Status, a SupportedValue, or a Result of one"
)
scry_add_reflection_compile_fail_test(
  duplicate-tool-name
  duplicate_tool_name.cpp
  "bool Lamp::toggle() and bool Lamp::switch_off() are both tools named \"toggle\""
)
scry_add_reflection_compile_fail_test(
  borrowed-toolbox
  borrowed_toolbox.cpp
  "Lamp is passed as an lvalue: ToolRegistry::add(Toolbox&&) takes ownership of a toolbox"
)
# Typed answers are tool inputs, so an answer type is held to ToolArguments.
scry_add_reflection_compile_fail_test(
  unsupported-answer
  unsupported_answer.cpp
  "Reading does not satisfy scry::reflection::ToolArguments: Reading::unit: char is a character type"
)
scry_add_reflection_compile_fail_test(
  scalar-answer
  scalar_answer.cpp
  "int does not satisfy scry::reflection::ToolArguments: int is not a plain aggregate"
)
scry_add_reflection_compile_fail_test(
  explicit-object-rvalue
  explicit_object_rvalue.cpp
  "Lamp::toggle takes its explicit object parameter by rvalue reference, so the toolbox object the registry holds cannot call it"
)
scry_add_reflection_compile_fail_test(
  explicit-object-const-toolbox
  explicit_object_const_toolbox.cpp
  "Lamp::toggle takes its explicit object parameter by non-const reference, and the toolbox is const"
)
scry_add_reflection_compile_fail_test(
  explicit-object-uncopyable
  explicit_object_uncopyable.cpp
  "Lamp::read takes its explicit object parameter by value, and Lamp cannot be copy-initialized from an lvalue of Lamp"
)
scry_add_reflection_compile_fail_test(
  unbound-member-tool
  unbound_member_tool.cpp
  "Lamp::toggle is a non-static member function; register an object of Lamp as a toolbox"
)
