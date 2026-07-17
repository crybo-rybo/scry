#!/usr/bin/env bash
# PostToolUse hook (Edit|Write): keep every edit format-clean and
# compile-clean so the CI format gate (clang-format --Werror) and the QA-006
# warnings-as-errors gate can never be a surprise at push time.
#
# Exit 0  = silent success.
# Exit 2  = feed stderr back to Claude (compile errors after an edit).
set -u

payload=$(cat)
if command -v jq >/dev/null 2>&1; then
    file=$(printf '%s' "$payload" | jq -r '.tool_input.file_path // empty')
else
    file=$(printf '%s' "$payload" | python3 -c \
        'import json,sys; print(json.load(sys.stdin).get("tool_input",{}).get("file_path",""))')
fi
[ -n "$file" ] || exit 0

root="${CLAUDE_PROJECT_DIR:-$(pwd)}"
rebuild=0
case "$file" in
    *.cpp | *.hpp)
        command -v clang-format >/dev/null 2>&1 && clang-format -i "$file"
        rebuild=1
        ;;
    *CMakeLists.txt | *.cmake | *CMakePresets.json)
        rebuild=1
        ;;
esac

# Incremental compile check — only when the dev tree is already configured.
if [ "$rebuild" -eq 1 ] && [ -d "$root/build/dev" ]; then
    if ! out=$(cd "$root" && cmake --build --preset dev 2>&1); then
        printf '%s\n' "$out" | tail -40 >&2
        exit 2
    fi
fi
exit 0
