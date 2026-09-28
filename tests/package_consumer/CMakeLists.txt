cmake_minimum_required(VERSION 3.28)

project(scry_package_consumer LANGUAGES CXX)

# No C++ modules; see the root CMakeLists.txt for why scanning is off.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# The testing component is optional in the install, so the consumer asks for it
# only when it is expected to be there. An install configured with
# SCRY_BUILD_TESTING_SUPPORT=OFF is exercised by turning this off too.
option(
  SCRY_CONSUMER_TESTING_COMPONENT
  "Require the installed scry::testing component"
  ON
)

# An install configured with SCRY_WITH_LLAMAD=ON links gRPC and Protobuf into
# scry::scry, and its package config must find them for this consumer; turning
# this on also checks that the llamad dialect is accepted.
option(
  SCRY_CONSUMER_LLAMAD
  "Require an install built with SCRY_WITH_LLAMAD=ON"
  OFF
)

if(SCRY_CONSUMER_TESTING_COMPONENT)
  find_package(scry 0.5.0 CONFIG REQUIRED COMPONENTS testing)
else()
  find_package(scry 0.5.0 CONFIG REQUIRED)
endif()
if(SCRY_CONSUMER_LLAMAD AND NOT scry_WITH_LLAMAD)
  message(FATAL_ERROR "the installed scry was built without SCRY_WITH_LLAMAD")
endif()

add_executable(scry_package_consumer main.cpp)
target_link_libraries(scry_package_consumer PRIVATE scry::scry)
if(SCRY_CONSUMER_TESTING_COMPONENT)
  target_link_libraries(scry_package_consumer PRIVATE scry::testing)
  target_compile_definitions(scry_package_consumer PRIVATE SCRY_CONSUMER_HAS_TESTING)
endif()
if(SCRY_CONSUMER_LLAMAD)
  target_compile_definitions(scry_package_consumer PRIVATE SCRY_CONSUMER_HAS_LLAMAD)
endif()
