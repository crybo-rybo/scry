# Contributing to Scry

Everything here is mechanical where it can be. If a rule cannot be checked by a
script, it is a habit, and it is named as one.

## Toolchain

Scry needs GCC 16 (for `-freflection`), CMake 3.28, Ninja, and libcurl.

**Linux:**

```sh
sudo add-apt-repository --yes ppa:ubuntu-toolchain-r/test
sudo apt-get update && sudo apt-get install -y g++-16 libcurl4-openssl-dev ninja-build
```

**macOS:**

```sh
brew install gcc llvm@18 ninja doxygen graphviz
```

Both platforms also need the complexity checker, which CI pins:

```sh
python3 -m pip install --user --break-system-packages lizard==1.22.1
```

`llvm@18` is optional but turns the clang-tidy leg on locally; CI pins
clang-tidy 18, so the keg-only formula is probed before the unversioned `llvm`.

## Presets

Build directories live under `build/<preset>`, Ninja, with
`compile_commands.json` exported; `.clangd` points at `build/dev`.

| Preset | For |
|---|---|
| `dev` | Debug. The everyday edit-build-test loop. |
| `ci` | RelWithDebInfo. What `ci-local.sh` builds, installs, and audits. |
| `asan` | Debug plus ASan and non-recovering UBSan. |
| `tsan` | Debug plus TSan; the actor model's "no shared mutable state" claim is exactly what erodes silently, and TSan is its enforcement. |
| `fuzz` | Clang with `SCRY_CLANG_TOOLING`, libFuzzer, ASan and UBSan. |

Every GCC preset pins `CMAKE_CXX_COMPILER` to `g++-16` through a hidden `gcc`
preset. Override it when your GCC 16 is spelled differently:
`cmake --preset dev -DCMAKE_CXX_COMPILER=/opt/homebrew/bin/g++-16`.

## The loop

```sh
cmake --preset dev                                # just configure
cmake --build build/dev                           # just build
ctest --test-dir build/dev --output-on-failure    # just test
```

Catch2 suites are registered with ctest under a per-suite prefix (`runtime.`,
`machine.`, `protocol.`, `provider.`, `transport.`, `integration.`,
`reflection.`); `public-api-contract` is a plain executable test.

```sh
ctest --test-dir build/dev -R 'runtime\.'                     # one suite
ctest --test-dir build/dev -R 'event queue coalesces'         # one case by name
./build/dev/tests/scry_runtime_tests "event queue coalesces adjacent deltas"
```

Formatting is clang-format, LLVM base, 88 columns:

```sh
cmake --build build/dev --target format         # just format
cmake --build build/dev --target format-check   # just format-check
```

## Gates

Every CI leg is one script under `scripts/`, and both the workflows and
`preflight.sh` call that same script, so a local gate and its hosted twin cannot
drift.

**Per commit** (`.github/workflows/ci.yml`):

| Job | Runs |
|---|---|
| Doxygen API site + clang-format | `./scripts/ci-docs.sh`, then a pinned `clang-format-18` dry run |
| Core, Linux GCC 16 and macOS GCC 16 | `./scripts/ci-local.sh` |
| clang-tidy | `./scripts/ci-tidy.sh` with `SCRY_TIDY_LIBCXX=1`, because Ubuntu 24.04's libstdc++ `<expected>` is newer than clang 18 can parse |
| ASan + UBSan, TSan | `./scripts/ci-sanitizer.sh asan` and `... tsan` |
| Fuzz corpus replay | `./scripts/ci-fuzz-replay.sh` |

**Weekly, Mondays** (`.github/workflows/nightly.yml`): CodeQL; a long fuzz run on
each of the five targets (`./scripts/ci-nightly-fuzz.sh <target>`); and the
showcase gate (`./scripts/ci-showcase.sh`). The end-to-end smoke against a real
local model (`./scripts/ci-local-model.sh`) is `workflow_dispatch` only — it
exercises a live model, not the deterministic protocol seams, so it never gates.

**On a tag** (`release.yml`): `check-release-tag.sh`, the core gate, the API
site, and the GitHub release built from the checked-in notes.

Run the whole per-commit ring locally before every pull request:

```sh
./scripts/preflight.sh    # just ci
```

It runs every leg, continues after failures, and reports a leg whose toolchain
this host cannot provide as `SKIP` rather than `FAIL`, naming every skipped leg
again in the closing summary — so the hosted-only gates for a change are explicit
rather than inferred from scrollback. Each sanitizer leg probes its own flag with
`g++-16` first, because GCC ships no thread-sanitizer runtime on Apple Silicon,
so TSan skips there while ASan still runs. `./scripts/ci-local.sh` (`just
ci-fast`) is the faster inner loop: diff check, complexity, unlinked TODOs,
format, build, tests, a staged install, and a downstream `find_package(scry)`
consumer.

The showcase is a standalone project under `extras/showcase/` that the root build
never configures; `./scripts/ci-showcase.sh` (`just showcase`) builds it, runs
the deterministic NPC and fake-panel tests and a real headless ImGui frame, then
audits that nothing it adds reached the installed package.

The five fuzz targets are `sse`, `anthropic`, `openai`, `response_policy`, and
`conversation`. Each replays its checked-in seed corpus per commit, so a target
cannot rot between long runs. For a long local search:

```sh
SCRY_NIGHTLY_FUZZ_SECONDS=1200 ./scripts/ci-nightly-fuzz.sh sse
```

## Testing

- **Test behavior at seams, not implementation inside them.** Tests target the
  machine, adapter, and transport interfaces. If refactoring internals breaks a
  test, the test was coupled to the wrong thing.
- **Fakes over mocks.** A hand-written fake transport with scriptable responses
  beats mock-framework expectations: fakes survive refactors and read as
  documentation. The seams are few and narrow enough to fake properly.
- **Determinism is non-negotiable.** No real sleeps, wall-clock time, or network
  in unit tests; time is an injected event, so a fake clock makes backoff
  testable to the millisecond. A flaky test is fixed or deleted the day it flakes.
- **Every bug becomes a test before it becomes a fix**, usually a machine-level
  event replay, committed with the fix permanently.
- **The machine suite is the bulk.** The sans-I/O loop is the most complex logic
  in the system and the cheapest to test exhaustively; the component, golden,
  integration, and showcase layers are thin above it.

## Mechanical limits

- Warnings are errors (`-Wall -Wextra -Wconversion -Wshadow`) on GCC 16 and on
  Clang under `SCRY_CLANG_TOOLING`. A warning on one compiler still fails.
- lizard: cyclomatic complexity fails at 15, argument count fails at 6, across
  `include src examples tests extras`.
- clang-tidy: cognitive complexity fails at 25, with a curated checked-in check
  list. It is attached to the library target, which is the whole Clang-analyzable
  surface — the `SCRY_CLANG_TOOLING` build compiles only the library and the fuzz
  targets, since the umbrella header includes reflection.
- `// TODO` must link an issue or a URL. CI rejects any unlinked TODO outright.

## Definition of done

- `./scripts/preflight.sh` ran, and any skipped legs are named.
- Tests are added or updated; a bug fix includes its regression test.
- [`docs/architecture.md`](architecture.md) is updated when behavior changes.
- An example compiles the change when the public API changes.
- A dependency change carries a written justification in the same commit.

## Pull requests

Trunk-based: short-lived branches, squash merge, conventional-commit messages,
`main` always green and always releasable. The pull-request template carries the
same three checkboxes as the definition of done.

## Releases

1. Bump `project(VERSION ...)` in `CMakeLists.txt`. That is the only place the
   release number lives; `<scry/version.hpp>` is generated from it.
2. Update the `find_package` version in `README.md` and
   `tests/package_consumer/CMakeLists.txt`, and the version string in
   `tests/public_api_contract.cpp`.
3. Write `docs/releases/vX.Y.Z.md`.
4. Check the tag first: `./scripts/check-release-tag.sh vX.Y.Z`.
5. Push the tag. The release workflow re-runs the core gate against the tagged
   tree, builds the API site, and publishes the release from those notes.
