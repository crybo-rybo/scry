# Cutting a release

Releases are tagged manually; no workflow runs on tags. The per-commit CI ring
on `main` is the release gate, and the tag marks the commit it validated.

1. **Verify the version.** `project(... VERSION x.y.z)` in `CMakeLists.txt`
   and the `SCRY_VERSION_*` macros in `include/scry/version.hpp` must agree —
   CMake refuses to configure when they diverge. For a new release, bump both
   together in the release pull request.
2. **Roll the changelog.** Move the `[Unreleased]` entries in
   [CHANGELOG.md](CHANGELOG.md) into a new dated `[x.y.z]` section, calling
   out every breaking change explicitly, and update the comparison links at
   the bottom of the file.
3. **Run the full local preflight** (`./scripts/preflight.sh`) and note any
   leg the host toolchain cannot provide; hosted CI is authoritative for
   those.
4. **Merge via pull request** and wait for the CI ring to pass on `main`.
   For a release touching the reflection component, confirm the path-aware
   GCC 16 leg ran on the pull request.
5. **Tag the merge commit** and push the tag:

   ```sh
   git tag -a vx.y.z -m "scry vx.y.z"
   git push origin vx.y.z
   ```

6. **Create the GitHub release** from the tag, using the changelog section as
   the release notes. The `scry-api-docs` artifact from the `main` CI run
   carries the matching API reference.
7. **Sanity-check downstream consumption.** The README's FetchContent snippet
   pins `GIT_TAG vx.y.z`; confirm a downstream `find_package(scry x.y.z)`
   consumer and a FetchContent build resolve against the new tag.
