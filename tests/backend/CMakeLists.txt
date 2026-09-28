add_executable(
  scry_backend_tests
  http_backend_tests.cpp
  model_backend_tests.cpp
)
target_include_directories(
  scry_backend_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(
  scry_backend_tests
  PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
)
catch_discover_tests(
  scry_backend_tests
  TEST_PREFIX "backend."
  PROPERTIES TIMEOUT 15
)

# The llamad backend against a scripted fake daemon on a private Unix socket.
# The fake is a real gRPC server built from the same generated service scry
# compiles, so these tests link gRPC and include the generated headers.
if(SCRY_WITH_LLAMAD)
  add_executable(
    scry_llamad_backend_tests
    llamad_backend_tests.cpp
    llamad_wire_tests.cpp
    ../support/llamad/fake_daemon.cpp
  )
  target_include_directories(
    scry_llamad_backend_tests
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/tests"
  )
  target_include_directories(
    scry_llamad_backend_tests
    SYSTEM PRIVATE "${SCRY_LLAMAD_GENERATED_DIR}"
  )
  # See cmake/ScryLlamad.cmake: <grpcpp/...> trips a #warning under C++26.
  set_source_files_properties(
    llamad_backend_tests.cpp
    ../support/llamad/fake_daemon.cpp
    PROPERTIES COMPILE_OPTIONS "${SCRY_LLAMAD_GRPC_COMPILE_OPTIONS}"
  )
  target_link_libraries(
    scry_llamad_backend_tests
    PRIVATE
      Catch2::Catch2WithMain
      scry::scry
      scry_project_options
      gRPC::grpc++
      protobuf::libprotobuf
  )
  set(SCRY_LLAMAD_TEST_PROPERTIES TIMEOUT 15)
  # The distribution's gRPC is not TSan-instrumented; see the suppression file.
  if(SCRY_SANITIZER STREQUAL "thread")
    list(
      APPEND
      SCRY_LLAMAD_TEST_PROPERTIES
      ENVIRONMENT
      "TSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tests/sanitizer/tsan_grpc.supp"
    )
  endif()
  catch_discover_tests(
    scry_llamad_backend_tests
    TEST_PREFIX "backend."
    PROPERTIES ${SCRY_LLAMAD_TEST_PROPERTIES}
  )
endif()
