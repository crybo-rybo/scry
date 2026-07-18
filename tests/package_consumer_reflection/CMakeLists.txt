cmake_minimum_required(VERSION 3.25)

project(scry_reflection_package_consumer LANGUAGES CXX)

find_package(scry CONFIG REQUIRED COMPONENTS reflection)

if(NOT TARGET scry::reflection)
  message(FATAL_ERROR "The requested scry reflection target is unavailable")
endif()

add_executable(scry_reflection_package_consumer main.cpp)
target_link_libraries(
  scry_reflection_package_consumer
  PRIVATE scry::reflection
)
