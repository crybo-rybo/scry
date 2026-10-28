# Scrydantic split: findings log

This log records how Scrydantic was carved out of Scry's reflection layer: the
couplings met, the decisions taken, and what a move to a repository of its own
still needs. It is written phase by phase, so later sections can revise
earlier ones.

## Phase log

### Phase 0: baseline

- Branch `refactor/scrydantic-split` from `main` @ 2e2553d.
- `./scripts/test.sh` (preset `dev`): 507/507 tests pass. The `ctest -N` list
  was saved for the final comparison (see "Test mapping" below).
- clang-format and clang-tidy 22.1.8 were installed from PyPI into a scratch
  virtual environment (`pip install 'clang-format==22.*' 'clang-tidy==22.*'`);
  the system tools are LLVM 18. Both scripts pass on the baseline.

### Phase 1: skeleton and boundary

- `scrydantic/CMakeLists.txt` is a project of its own (`project(scrydantic)`),
  added by the root with `add_subdirectory(scrydantic)`. Scry passes its
  options down as `SCRYDANTIC_BUILD_TESTS`, `SCRYDANTIC_BUILD_EXAMPLES`,
  `SCRYDANTIC_WARNINGS_AS_ERRORS`, and `SCRYDANTIC_SANITIZER`, so the `asan` and
  `tsan` presets instrument Scrydantic too.
- `include(CTest)` moved above `add_subdirectory(scrydantic)` in the root, so
  Scrydantic's tests register in the same ctest tree.
- Scrydantic has its own warning target (`scrydantic_project_options`), a copy
  of `scry_project_options`; neither is exported.
- `scripts/lint.sh` rejects any `#include` of `<scry/...>`, `<curl/...>`, or a
  Scry `src/` header under `scrydantic/`, and any `CURL::`/`Threads::` target
  there. `scripts/format.sh` covers `scrydantic/`.
- Scrydantic is versioned on its own, starting at 0.1.0. The root passes the
  version to `scryConfig.cmake`, which does `find_dependency(scrydantic 0.1.0)`.
