if(NOT DEFINED PROBE OR PROBE STREQUAL "")
  message(FATAL_ERROR "PROBE must name the UBSan fatality executable")
endif()

execute_process(
  COMMAND "${PROBE}"
  RESULT_VARIABLE probe_result
  OUTPUT_VARIABLE probe_output
  ERROR_VARIABLE probe_error
)

if("${probe_result}" STREQUAL "0")
  message(
    FATAL_ERROR
    "UBSan recovered instead of failing:\n${probe_output}${probe_error}"
  )
endif()

message(STATUS "UBSan terminated the probe as required: ${probe_result}")
