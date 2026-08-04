if(NOT DEFINED PROBE OR NOT DEFINED MODE OR NOT DEFINED WORK_DIRECTORY)
  message(FATAL_ERROR "PROBE, MODE, and WORK_DIRECTORY are required")
endif()

file(REMOVE_RECURSE "${WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
set(EXPLICIT_LOG "${WORK_DIRECTORY}/explicit.log")

if(MODE STREQUAL "unset")
  set(LOGGING_ENV --unset=SCRY_LOG_FILE)
elseif(MODE STREQUAL "empty")
  set(LOGGING_ENV "SCRY_LOG_FILE=")
elseif(MODE STREQUAL "explicit")
  set(LOGGING_ENV "SCRY_LOG_FILE=${EXPLICIT_LOG}")
else()
  message(FATAL_ERROR "unknown logging mode: ${MODE}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${LOGGING_ENV} "${PROBE}"
  WORKING_DIRECTORY "${WORK_DIRECTORY}"
  RESULT_VARIABLE PROBE_RESULT
)
if(NOT PROBE_RESULT EQUAL 0)
  message(FATAL_ERROR "logging probe failed with exit code ${PROBE_RESULT}")
endif()

if(MODE STREQUAL "explicit")
  if(NOT EXISTS "${EXPLICIT_LOG}")
    message(FATAL_ERROR "an explicit SCRY_LOG_FILE destination was not created")
  endif()
  file(READ "${EXPLICIT_LOG}" LOG_CONTENT)
  string(FIND "${LOG_CONTENT}" "logging-probe-marker" MARKER_POSITION)
  if(MARKER_POSITION EQUAL -1)
    message(FATAL_ERROR "the explicit log does not contain the probe marker")
  endif()
else()
  file(GLOB CREATED_FILES LIST_DIRECTORIES false "${WORK_DIRECTORY}/*")
  if(CREATED_FILES)
    message(FATAL_ERROR "logging without an explicit destination created: ${CREATED_FILES}")
  endif()
endif()
