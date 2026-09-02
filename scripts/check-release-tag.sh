#!/usr/bin/env bash

# Refuse a release tag that does not name the version in CMakeLists.txt, which
# is the only place the release number lives. Run by the release workflow and
# locally before tagging.
#
# Usage: check-release-tag.sh v<major>.<minor>.<patch>

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly tag="${1:-}"

if [[ -z "${tag}" ]]; then
  echo "Usage: $0 v<major>.<minor>.<patch>" >&2
  exit 2
fi

project_version="$(
  sed -n -E 's/^[[:space:]]*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+)[[:space:]]*$/\1/p' \
    "${root_dir}/CMakeLists.txt" | head -n 1
)"
readonly project_version

if [[ -z "${project_version}" ]]; then
  echo "Could not read the project version from CMakeLists.txt" >&2
  exit 1
fi

if [[ "${tag}" != "v${project_version}" ]]; then
  echo "Tag ${tag} does not match the project version v${project_version}" >&2
  exit 1
fi

echo "Tag ${tag} matches the project version."
