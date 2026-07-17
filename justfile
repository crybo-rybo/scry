# Scry developer commands. `just ci-fast` mirrors the per-commit CI ring
# (QA-011: everything CI enforces is runnable locally with one command).

default:
    @just --list

# Configure a CMake preset (dev | release | asan-ubsan | tsan | reflection).
configure preset="dev":
    cmake --preset {{preset}}

build preset="dev": (configure preset)
    cmake --build --preset {{preset}}

test preset="dev": (build preset)
    ctest --preset {{preset}}

format:
    git ls-files '*.cpp' '*.hpp' | xargs clang-format -i

format-check:
    git ls-files '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror

tidy: (configure "dev")
    run-clang-tidy -quiet -p build/dev $(git ls-files '*.cpp')

# The per-commit CI ring, locally (QA-011).
ci-fast: format-check (test "dev") (test "asan-ubsan") (test "tsan") tidy

clean:
    rm -rf build
