cmake_minimum_required(VERSION 3.25)

project(scry_package_consumer LANGUAGES CXX)

find_package(scry CONFIG REQUIRED)

add_executable(scry_package_consumer main.cpp)
target_link_libraries(scry_package_consumer PRIVATE scry::scry)
