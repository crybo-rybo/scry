cmake_minimum_required(VERSION 3.28)

project(scry_package_consumer LANGUAGES CXX)

# No C++ modules; see the root CMakeLists.txt for why scanning is off.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(scry 0.2.0 CONFIG REQUIRED)

add_executable(scry_package_consumer main.cpp)
target_link_libraries(scry_package_consumer PRIVATE scry::scry)
