include(FetchContent)
# Under Scry this is the first declaration of Catch2, so Scry's tests reuse it;
# both must name the same revision.
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG 317ac1ed4c0bb6e6b91eafc817e05c488feffcb3
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Catch2)
foreach(SCRYDANTIC_CATCH_TARGET IN ITEMS Catch2 Catch2WithMain)
  target_compile_features("${SCRYDANTIC_CATCH_TARGET}" PRIVATE cxx_std_23)
endforeach()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# Every public header must compile on its own, so each one becomes a lone
# translation unit that includes nothing else, on an object target of its own
# per library: the JSON core's headers as C++23, the rest as C++26.
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated/header_checks")
function(scrydantic_add_header_checks TARGET LIBRARY)
  add_library("${TARGET}" OBJECT)
  # Keep one translation unit per header even in a unity build.
  set_target_properties("${TARGET}" PROPERTIES UNITY_BUILD OFF)
  target_link_libraries("${TARGET}" PRIVATE "${LIBRARY}" scrydantic_project_options)
  foreach(SCRYDANTIC_HEADER IN LISTS ARGN)
    string(REGEX REPLACE "^include/" "" SCRYDANTIC_HEADER_INCLUDE "${SCRYDANTIC_HEADER}")
    string(MAKE_C_IDENTIFIER "${SCRYDANTIC_HEADER_INCLUDE}" SCRYDANTIC_HEADER_ID)
    set(
      SCRYDANTIC_HEADER_CHECK_SOURCE
      "${PROJECT_BINARY_DIR}/generated/header_checks/${SCRYDANTIC_HEADER_ID}.cpp"
    )
    file(
      GENERATE
      OUTPUT "${SCRYDANTIC_HEADER_CHECK_SOURCE}"
      CONTENT "#include <${SCRYDANTIC_HEADER_INCLUDE}>\n"
    )
    target_sources("${TARGET}" PRIVATE "${SCRYDANTIC_HEADER_CHECK_SOURCE}")
  endforeach()
endfunction()

scrydantic_add_header_checks(
  scrydantic_json_header_checks scrydantic::json ${SCRYDANTIC_JSON_PUBLIC_HEADERS}
)
# The JSON core's headers stay C++23 without reflection, as its sources are.
set_target_properties(
  scrydantic_json_header_checks
  PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
)
target_compile_options(scrydantic_json_header_checks PRIVATE -Werror=c++26-extensions)
scrydantic_add_header_checks(
  scrydantic_header_checks scrydantic::scrydantic ${SCRYDANTIC_PUBLIC_HEADERS}
)

# The JSON core's own tests, compiled as C++23 like the core itself. They reach
# the document type behind JsonView through the private src/ headers.
add_executable(
  scrydantic_json_tests
  json/json_codec_validation_tests.cpp
  json/json_document_tests.cpp
  json/json_fixture_tests.cpp
  json/json_view_tests.cpp
)
target_compile_features(scrydantic_json_tests PRIVATE cxx_std_23)
target_include_directories(scrydantic_json_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
# The JSON golden fixtures are frozen, generated text, checked in as one archive
# (fixtures/json/README.md) and unpacked at configure time. The corpora supply
# the prefixes the truncation replay walks.
set(
  SCRYDANTIC_JSON_FIXTURE_ARCHIVE
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/json/goldens.tar.xz"
)
set(SCRYDANTIC_JSON_FIXTURE_DIR "${PROJECT_BINARY_DIR}/tests/fixtures/json")
file(
  ARCHIVE_EXTRACT
  INPUT "${SCRYDANTIC_JSON_FIXTURE_ARCHIVE}"
  DESTINATION "${SCRYDANTIC_JSON_FIXTURE_DIR}"
)
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${SCRYDANTIC_JSON_FIXTURE_ARCHIVE}"
)
target_compile_definitions(
  scrydantic_json_tests
  PRIVATE
    "SCRYDANTIC_JSON_FIXTURE_DIR=\"${SCRYDANTIC_JSON_FIXTURE_DIR}\""
    "SCRYDANTIC_FUZZ_CORPUS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus\""
)
target_link_libraries(
  scrydantic_json_tests
  PRIVATE Catch2::Catch2WithMain scrydantic::json scrydantic_project_options
)
catch_discover_tests(scrydantic_json_tests TEST_PREFIX "scrydantic.json.")

# The JSON fuzz target is a libFuzzer-style LLVMFuzzerTestOneInput entry point
# over the checked-in seed corpus under fuzz/corpus/json. fuzz/replay_main.cpp
# runs every seed through it once, so the corpus is an ordinary regression test
# that a sanitizer build also runs under ASan and UBSan.
add_executable(
  scrydantic_json_fuzz_replay
  fuzz/replay_main.cpp
  fuzz/json_fuzz.cpp
)
target_compile_features(scrydantic_json_fuzz_replay PRIVATE cxx_std_23)
target_include_directories(
  scrydantic_json_fuzz_replay
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
  scrydantic_json_fuzz_replay
  PRIVATE scrydantic::json scrydantic_project_options
)
add_test(
  NAME scrydantic.json-fuzz-replay
  COMMAND scrydantic_json_fuzz_replay "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/json"
)
set_tests_properties(scrydantic.json-fuzz-replay PROPERTIES TIMEOUT 45)

# The reflected codec, schemas, and tool binding, linked against Scrydantic and
# Catch2 alone.
add_executable(
  scrydantic_tests
  codec/codec_tests.cpp
  tools/tool_binding_tests.cpp
)
target_link_libraries(
  scrydantic_tests
  PRIVATE Catch2::Catch2WithMain scrydantic::scrydantic scrydantic_project_options
)
catch_discover_tests(scrydantic_tests TEST_PREFIX "scrydantic.")

# A binding must generate code from the declarations its own translation unit
# sees (a namespace's tools, a function's parameter names and annotations),
# however the linker orders the units, so the same program is linked in both
# orders.
foreach(SCRYDANTIC_LINK_ORDER IN ITEMS a_then_b b_then_a)
  if(SCRYDANTIC_LINK_ORDER STREQUAL "a_then_b")
    set(SCRYDANTIC_MULTI_TU_UNITS tools_a.cpp tools_b.cpp)
  else()
    set(SCRYDANTIC_MULTI_TU_UNITS tools_b.cpp tools_a.cpp)
  endif()
  list(TRANSFORM SCRYDANTIC_MULTI_TU_UNITS PREPEND "multi_tu/")
  add_executable(
    scrydantic_multi_tu_${SCRYDANTIC_LINK_ORDER}
    multi_tu/main.cpp
    ${SCRYDANTIC_MULTI_TU_UNITS}
  )
  target_link_libraries(
    scrydantic_multi_tu_${SCRYDANTIC_LINK_ORDER}
    PRIVATE scrydantic::scrydantic scrydantic_project_options
  )
  add_test(
    NAME "scrydantic.multi-tu-${SCRYDANTIC_LINK_ORDER}"
    COMMAND scrydantic_multi_tu_${SCRYDANTIC_LINK_ORDER}
  )
endforeach()

# Each rejection names the type, member, function, or namespace at fault and the
# reason, with only Scrydantic's headers on the include path.
function(
  scrydantic_add_compile_fail_test
  TEST_NAME
  SOURCE_NAME
  EXPECTED_SUBSTRING
)
  add_test(
    NAME "scrydantic.compile-fail.${TEST_NAME}"
    COMMAND
      "${CMAKE_COMMAND}"
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/${SOURCE_NAME}"
      "-DPROJECT_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
      "-DEXPECTED_SUBSTRING=${EXPECTED_SUBSTRING}"
      -P "${PROJECT_SOURCE_DIR}/cmake/ExpectCompileFailure.cmake"
  )
endfunction()

scrydantic_add_compile_fail_test(
  duplicate-description
  duplicate_description.cpp
  "Arguments::value: a reflected member may have at most one scrydantic::description annotation"
)
scrydantic_add_compile_fail_test(
  unsupported-schema-root
  unsupported_schema_root.cpp
  "does not satisfy scrydantic::SupportedValue: scrydantic::Json is raw JSON, which has no schema"
)
scrydantic_add_compile_fail_test(
  untagged-variant
  untagged_variant.cpp
  "Arguments::shape: std::variant<Circle, Square> has the alternative Circle without a scrydantic::tag annotation"
)
scrydantic_add_compile_fail_test(
  duplicate-key
  duplicate_key.cpp
  "does not satisfy scrydantic::Encodable: Limits has members limit and max_tokens that both map to the JSON key"
)
scrydantic_add_compile_fail_test(
  bind-misplaced-tool
  bind_misplaced_tool.cpp
  "scrydantic::bind<^^lamp_tools>(): lamp_tools::brightness carries scrydantic::tool, which applies to functions"
)
scrydantic_add_compile_fail_test(
  misplaced-context
  misplaced_context.cpp
  "greeting_tools::greet has parameter `session` of type Session; the context is only ever the leading parameter, as const Session&"
)
