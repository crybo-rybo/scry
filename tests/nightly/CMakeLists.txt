# The local-model smoke drives a live OpenAI-compatible server through the
# public API. It is a plain executable with no ctest registration on purpose:
# scripts/ci-local-model.sh runs it under an explicit timeout in the on-demand
# local-model job of .github/workflows/nightly.yml, and no per-commit gate
# depends on a live model.
add_executable(scry_local_model_smoke EXCLUDE_FROM_ALL local_model_smoke.cpp)
target_link_libraries(
  scry_local_model_smoke
  PRIVATE scry::scry scry_project_options
)

# Scenarios against a real llamad daemon and model, run by hand; see the file.
if(SCRY_WITH_LLAMAD)
  add_executable(scry_llamad_e2e EXCLUDE_FROM_ALL llamad_e2e.cpp)
  target_link_libraries(scry_llamad_e2e PRIVATE scry::scry scry_project_options)
endif()
