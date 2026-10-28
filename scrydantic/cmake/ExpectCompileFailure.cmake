# cmake -DCXX_COMPILER=... -DSOURCE_FILE=... -DPROJECT_INCLUDE_DIR=...
#       -DEXPECTED_SUBSTRING=... -P ExpectCompileFailure.cmake
#
# Passes when SOURCE_FILE fails to compile with a diagnostic containing
# EXPECTED_SUBSTRING. -fsyntax-only still instantiates templates and
# constant-evaluates, so the reflection diagnostics fire without an object file.
execute_process(
  COMMAND
    "${CXX_COMPILER}" -std=c++26 -freflection "-I${PROJECT_INCLUDE_DIR}"
    -fsyntax-only "${SOURCE_FILE}"
  RESULT_VARIABLE compile_result
  OUTPUT_VARIABLE compiler_stdout
  ERROR_VARIABLE compiler_stderr)

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
