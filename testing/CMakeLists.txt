# scry::testing drives the shipping runtime over a scripted transport, so it
# links the library rather than recompiling any part of it. It carries no
# reflection syntax and reaches the transport seam through private headers,
# which is exactly what the SCRY_CLANG_TOOLING build can still compile, so it is
# built there too.
add_library(scry_testing STATIC src/scripted_transport.cpp)
add_library(scry::testing ALIAS scry_testing)
set_target_properties(
  scry_testing
  PROPERTIES
    EXPORT_NAME testing
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(
  scry_testing
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
  scry_testing
  PUBLIC scry::scry
  PRIVATE
    Threads::Threads
    "$<BUILD_INTERFACE:scry_project_options>"
)
target_sources(
  scry_testing
  PUBLIC
    FILE_SET scry_testing_headers
    TYPE HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES ${SCRY_TESTING_PUBLIC_HEADERS}
)

# The component ships in the same export set as scry::scry: the config file
# reports it as found when the installed targets file defines it, which is the
# only way an install configured with SCRY_BUILD_TESTING_SUPPORT=OFF can be told
# apart from one that carries it.
install(
  TARGETS scry_testing
  EXPORT scryTargets
  FILE_SET scry_testing_headers
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
