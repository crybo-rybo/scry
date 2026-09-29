# Tests against a live model server, none registered with ctest because no
# build is guaranteed a model. Each is excluded from all and built by its
# script: scripts/ci-local-model.sh for the OpenAI-compatible smoke against
# Ollama, scripts/e2e-llamad.sh for the llamad suite.
add_executable(scry_local_model_smoke EXCLUDE_FROM_ALL local_model_smoke.cpp)
target_link_libraries(
  scry_local_model_smoke
  PRIVATE scry::scry scry_project_options
)

if(SCRY_WITH_LLAMAD)
  add_executable(scry_llamad_e2e_tests EXCLUDE_FROM_ALL llamad_e2e_tests.cpp)
  target_link_libraries(
    scry_llamad_e2e_tests
    PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
  )
endif()
