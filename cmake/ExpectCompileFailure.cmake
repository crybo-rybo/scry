foreach(required_variable
        IN ITEMS CXX_COMPILER SOURCE_FILE PROJECT_INCLUDE_DIR EXPECTED_SUBSTRING)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} must be provided")
  endif()
endforeach()

if(NOT EXISTS "${CXX_COMPILER}")
  message(FATAL_ERROR "CXX_COMPILER does not exist: ${CXX_COMPILER}")
endif()

if(NOT EXISTS "${SOURCE_FILE}")
  message(FATAL_ERROR "SOURCE_FILE does not exist: ${SOURCE_FILE}")
endif()

if(NOT IS_DIRECTORY "${PROJECT_INCLUDE_DIR}")
  message(FATAL_ERROR
          "PROJECT_INCLUDE_DIR is not a directory: ${PROJECT_INCLUDE_DIR}")
endif()

if(NOT DEFINED OUTPUT_DIRECTORY OR "${OUTPUT_DIRECTORY}" STREQUAL "")
  set(OUTPUT_DIRECTORY
      "${CMAKE_CURRENT_BINARY_DIR}/scry-reflection-compile-fail")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
string(MD5 fixture_id "${SOURCE_FILE};${EXPECTED_SUBSTRING}")
set(object_file "${OUTPUT_DIRECTORY}/${fixture_id}.o")

execute_process(
  COMMAND
    "${CXX_COMPILER}" -std=c++26 -freflection
    -DSCRY_ENABLE_REFLECTION=1 "-I${PROJECT_INCLUDE_DIR}" -c "${SOURCE_FILE}" -o
    "${object_file}"
  RESULT_VARIABLE compile_result
  OUTPUT_VARIABLE compiler_stdout
  ERROR_VARIABLE compiler_stderr)

file(REMOVE "${object_file}")

set(compiler_output "${compiler_stdout}\n${compiler_stderr}")

if(compile_result EQUAL 0)
  message(FATAL_ERROR
          "Expected compilation to fail, but it succeeded for ${SOURCE_FILE}")
endif()

string(FIND "${compiler_output}" "${EXPECTED_SUBSTRING}" substring_position)
if(substring_position EQUAL -1)
  message(
    FATAL_ERROR
      "Compilation failed without the expected diagnostic substring.\n"
      "Source: ${SOURCE_FILE}\n"
      "Expected: ${EXPECTED_SUBSTRING}\n"
      "Compiler output:\n${compiler_output}")
endif()

message(STATUS
        "Observed expected compile failure for ${SOURCE_FILE}: "
        "${EXPECTED_SUBSTRING}")
