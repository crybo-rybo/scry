# Exercise the installed Scry and Scrydantic packages without any source includes.
function(run)
  execute_process(
    COMMAND ${ARGV}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${ARGV}\n${output}\n${error}")
  endif()
endfunction()

set(work_dir "${SCRY_BINARY_DIR}/package-smoke")
file(REMOVE_RECURSE "${work_dir}")
set(prefix "${work_dir}/stage")
# Use an initial cache file so a list of prefixes remains one CMake value.
file(
  WRITE "${work_dir}/dependency-paths.cmake"
  "set(CMAKE_PREFIX_PATH [==[${prefix};${DEPENDENCY_PREFIX_PATH}]==] CACHE STRING \"\")\n"
)
run("${CMAKE_COMMAND}" --install "${SCRY_BINARY_DIR}" --prefix "${prefix}" --config "${CONFIG}")

run(
  "${CMAKE_COMMAND}" -S "${SCRY_SOURCE_DIR}/tests/package" -B "${work_dir}/consumer"
  -G Ninja -C "${work_dir}/dependency-paths.cmake"
  "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-Dscrydantic_DIR=${DEPENDENCY_DIR}"
  "-DCMAKE_BUILD_TYPE=${CONFIG}" "-DTESTING_SUPPORT=${TESTING_SUPPORT}"
  "-DSANITIZER=${SANITIZER}"
)
run("${CMAKE_COMMAND}" --build "${work_dir}/consumer")
run("${CMAKE_CTEST_COMMAND}" --test-dir "${work_dir}/consumer" --output-on-failure)

# Reconfigure Scry against an installed Scrydantic with fetching disabled. This
# also checks that scryConfig's dependency version does not rely on FetchContent.
run(
  "${CMAKE_COMMAND}" -S "${SCRY_SOURCE_DIR}" -B "${work_dir}/installed-dependency"
  -G Ninja -C "${work_dir}/dependency-paths.cmake"
  "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-Dscrydantic_DIR=${DEPENDENCY_DIR}"
  -DSCRY_FETCH_SCRYDANTIC=OFF -DSCRY_BUILD_TESTS=OFF -DSCRY_BUILD_EXAMPLES=OFF
  "-DSCRY_BUILD_TESTING_SUPPORT=${TESTING_SUPPORT}"
)
