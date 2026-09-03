# The local-model smoke drives a live OpenAI-compatible server through the
# public API. It is a plain executable with no ctest registration on purpose:
# scripts/ci-local-model.sh runs it under an explicit timeout in the
# workflow_dispatch nightly, and no per-commit gate depends on a live model.
add_executable(scry_local_model_smoke local_model_smoke.cpp)
target_link_libraries(
  scry_local_model_smoke
  PRIVATE scry::scry scry_project_options
)
