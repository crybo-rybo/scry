# The end-to-end smoke drives a live Anthropic or OpenAI-compatible server
# through the public API. It is a plain executable with no ctest registration on
# purpose: scripts/test-e2e.sh builds and runs it on demand.
# No per-commit gate depends on a live model.
add_executable(scry_e2e_smoke EXCLUDE_FROM_ALL live_smoke.cpp)
target_link_libraries(
  scry_e2e_smoke
  PRIVATE scry::scry scry_project_options
)
