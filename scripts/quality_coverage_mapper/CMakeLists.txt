cmake_minimum_required(VERSION 3.25)

project(scry_quality_coverage_mapper LANGUAGES CXX)

find_package(scry CONFIG REQUIRED)

if(NOT DEFINED SCRY_SOURCE_ROOT)
  message(FATAL_ERROR "SCRY_SOURCE_ROOT is required")
endif()

set(SCRY_PUBLIC_API_CONTRACT "${SCRY_SOURCE_ROOT}/tests/public_api_contract.cpp")
set_source_files_properties(
  "${SCRY_PUBLIC_API_CONTRACT}"
  PROPERTIES COMPILE_DEFINITIONS "main=scry_public_api_contract_main"
)

add_executable(
  scry_quality_coverage_mapper
  main.cpp
  "${SCRY_PUBLIC_API_CONTRACT}"
)
target_compile_features(scry_quality_coverage_mapper PRIVATE cxx_std_23)
target_include_directories(
  scry_quality_coverage_mapper
  BEFORE PRIVATE "${SCRY_SOURCE_ROOT}/include"
)
target_link_libraries(
  scry_quality_coverage_mapper
  PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,scry::scry>"
)
