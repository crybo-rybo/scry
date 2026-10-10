# Fails unless the README's quickstart block, the first ```cpp fence after the
# marker comment, is byte-for-byte equal to examples/quickstart.cpp.
# Usage: cmake -DREADME=<README.md> -DEXAMPLE=<quickstart.cpp> -P readme_quickstart.cmake

file(READ "${README}" readme)
file(READ "${EXAMPLE}" example)

set(marker "<!-- examples/quickstart.cpp -->")
string(FIND "${readme}" "${marker}" marker_at)
if(marker_at EQUAL -1)
  message(FATAL_ERROR "README.md has no '${marker}' marker")
endif()
string(SUBSTRING "${readme}" ${marker_at} -1 readme)

set(fence_open "```cpp\n")
string(FIND "${readme}" "${fence_open}" open_at)
if(open_at EQUAL -1)
  message(FATAL_ERROR "README.md has no ```cpp block after the quickstart marker")
endif()
string(LENGTH "${fence_open}" fence_open_length)
math(EXPR body_at "${open_at} + ${fence_open_length}")
string(SUBSTRING "${readme}" ${body_at} -1 readme)

string(FIND "${readme}" "\n```" close_at)
if(close_at EQUAL -1)
  message(FATAL_ERROR "README.md's quickstart block is not closed")
endif()
math(EXPR body_length "${close_at} + 1")
string(SUBSTRING "${readme}" 0 ${body_length} block)

if(NOT block STREQUAL example)
  message(FATAL_ERROR
    "README.md's quickstart block differs from ${EXAMPLE}. Copy the file into the block.")
endif()
