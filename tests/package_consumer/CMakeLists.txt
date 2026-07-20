cmake_minimum_required(VERSION 3.25)

project(scry_package_consumer LANGUAGES CXX)

find_package(scry CONFIG REQUIRED OPTIONAL_COMPONENTS reflection)

if(scry_reflection_FOUND OR TARGET scry::reflection)
  message(FATAL_ERROR "The reflection component leaked into a core-only package")
endif()

# Compile-only mode audits scry::scry's interface (flags, includes) with a
# compiler that cannot link an archive built by a newer GCC: that link needs
# the newer libstdc++ regardless of what the package exposes, so it proves
# nothing about leakage.
option(
  SCRY_CONSUMER_COMPILE_ONLY
  "Compile against scry::scry without linking an executable"
  OFF
)

if(SCRY_CONSUMER_COMPILE_ONLY)
  add_library(scry_package_consumer OBJECT main.cpp)
else()
  add_executable(scry_package_consumer main.cpp)
endif()
target_link_libraries(scry_package_consumer PRIVATE scry::scry)
