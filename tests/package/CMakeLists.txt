cmake_minimum_required(VERSION 3.30)
project(scry_package_consumer LANGUAGES CXX)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(TESTING_SUPPORT)
  find_package(scry CONFIG REQUIRED COMPONENTS testing)
else()
  find_package(scry CONFIG REQUIRED)
endif()

add_executable(scry_package_consumer main.cpp)
target_link_libraries(scry_package_consumer PRIVATE scry::scry)
if(TESTING_SUPPORT)
  target_link_libraries(scry_package_consumer PRIVATE scry::testing)
  target_compile_definitions(scry_package_consumer PRIVATE TESTING_SUPPORT)
endif()

# Sanitizer flags are deliberately not part of Scry's installed interface.
if(SANITIZER STREQUAL "address-undefined")
  target_compile_options(scry_package_consumer PRIVATE -fsanitize=address,undefined)
  target_link_options(scry_package_consumer PRIVATE -fsanitize=address,undefined)
elseif(SANITIZER STREQUAL "thread")
  target_compile_options(scry_package_consumer PRIVATE -fsanitize=thread)
  target_link_options(scry_package_consumer PRIVATE -fsanitize=thread)
endif()

enable_testing()
add_test(NAME installed-consumer COMMAND scry_package_consumer)
