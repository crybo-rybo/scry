#!/usr/bin/env bash

set -euo pipefail

readonly tooling_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
Usage: scripts/perf-pair.sh --base-dir DIR --head-dir DIR --output DIR [options]

Build two revisions separately, then collect fresh-process A-B-B-A samples with
the HEAD checkout's profiling scripts as the common orchestrator.

Options:
  --base-dir DIR         comparison-base source checkout (required)
  --head-dir DIR         candidate/HEAD source checkout (required)
  --output DIR           paired artifact directory (required)
  --filter REGEX         optional Google Benchmark filter
  --cycles COUNT         A-B-B-A cycles (default: 5; 10 samples/revision)
  --mode MODE            full (default) or smoke
  --allow-dirty          allow diagnostic comparison of dirty source/tooling
  --informational-reason TEXT
                         mark results ineligible for review evidence
  --help                 show this help

Smoke mode defaults to one cycle and is never evidence-eligible. A full clean
five-cycle manifest is required before the comparator marks protocol evidence
eligible; timing percentages remain non-gating.
USAGE
}

base_dir=""
head_dir=""
output_dir=""
benchmark_filter=""
cycles="5"
cycles_explicit=false
mode="full"
allow_dirty=false
informational_reason=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base-dir)
      [[ $# -ge 2 ]] || { echo "--base-dir requires a value" >&2; exit 2; }
      base_dir="$2"
      shift 2
      ;;
    --head-dir)
      [[ $# -ge 2 ]] || { echo "--head-dir requires a value" >&2; exit 2; }
      head_dir="$2"
      shift 2
      ;;
    --output)
      [[ $# -ge 2 ]] || { echo "--output requires a value" >&2; exit 2; }
      output_dir="$2"
      shift 2
      ;;
    --filter)
      [[ $# -ge 2 ]] || { echo "--filter requires a value" >&2; exit 2; }
      benchmark_filter="$2"
      shift 2
      ;;
    --cycles)
      [[ $# -ge 2 ]] || { echo "--cycles requires a value" >&2; exit 2; }
      cycles="$2"
      cycles_explicit=true
      shift 2
      ;;
    --mode)
      [[ $# -ge 2 ]] || { echo "--mode requires a value" >&2; exit 2; }
      mode="$2"
      shift 2
      ;;
    --allow-dirty)
      allow_dirty=true
      shift
      ;;
    --informational-reason)
      [[ $# -ge 2 ]] || { echo "--informational-reason requires a value" >&2; exit 2; }
      informational_reason="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "${base_dir}" || -z "${head_dir}" || -z "${output_dir}" ]]; then
  echo "--base-dir, --head-dir, and --output are required" >&2
  usage >&2
  exit 2
fi
case "${mode}" in
  full) ;;
  smoke)
    if [[ "${cycles_explicit}" == false ]]; then
      cycles="1"
    fi
    ;;
  *)
    echo "--mode must be full or smoke" >&2
    exit 2
    ;;
esac
if [[ ! "${cycles}" =~ ^[1-9][0-9]*$ ]]; then
  echo "--cycles must be a positive integer" >&2
  exit 2
fi

base_dir="$(cd "${base_dir}" && pwd)"
head_dir="$(cd "${head_dir}" && pwd)"
if [[ "${output_dir}" != /* ]]; then
  output_dir="${tooling_dir}/${output_dir}"
fi
if [[ -d "${output_dir}" ]]; then
  existing_artifact="$(find "${output_dir}" -type f -print -quit)"
  if [[ -n "${existing_artifact}" ]]; then
    echo "Refusing to overwrite non-empty paired artifact directory: ${output_dir}" >&2
    exit 2
  fi
fi

readonly tooling_commit="$(git -C "${tooling_dir}" rev-parse HEAD)"
readonly head_commit="$(git -C "${head_dir}" rev-parse HEAD)"
if [[ "${tooling_commit}" != "${head_commit}" ]]; then
  echo "perf-pair.sh must come from the candidate/HEAD revision" >&2
  echo "tooling=${tooling_commit} head=${head_commit}" >&2
  exit 2
fi

if [[ "${allow_dirty}" == false ]]; then
  dirty_contexts=""
  record_dirty_context() {
    local label="$1"
    local directory="$2"
    local status=""
    status="$(git -C "${directory}" status --porcelain=v1 --untracked-files=normal)"
    if [[ -n "${status}" ]]; then
      dirty_contexts="${dirty_contexts}${dirty_contexts:+, }${label}"
    fi
  }
  record_dirty_context base "${base_dir}"
  record_dirty_context head "${head_dir}"
  record_dirty_context tooling "${tooling_dir}"
  if [[ -n "${dirty_contexts}" ]]; then
    echo "Refusing dirty paired inputs before configure/build: ${dirty_contexts}" >&2
    echo "Use --allow-dirty for diagnostic, evidence-ineligible output." >&2
    exit 2
  fi
fi

readonly base_build_dir="${base_dir}/build/profile-pair-base"
readonly head_build_dir="${head_dir}/build/profile-pair-head"

prepare_revision() {
  local source_dir="$1"
  local build_dir="$2"
  (cd "${source_dir}" && cmake --preset profile -B "${build_dir}")
  cmake --build "${build_dir}" \
    --target scry_timing_benchmarks scry_allocation_benchmarks
}

prepare_revision "${base_dir}" "${base_build_dir}"
prepare_revision "${head_dir}" "${head_build_dir}"
mkdir -p "${output_dir}/runs"

run_revision() {
  local revision="$1"
  local source_dir="$2"
  local build_dir="$3"
  local artifact_dir="$4"
  local -a arguments=(
    --source-dir "${source_dir}"
    --build-dir "${build_dir}"
    --skip-configure
    --skip-build
    --mode "${mode}"
    --repetitions 1
    --output "${artifact_dir}"
  )
  if [[ -n "${benchmark_filter}" ]]; then
    arguments+=(--filter "${benchmark_filter}")
  fi
  echo "Paired sample ${revision}: ${artifact_dir}"
  "${tooling_dir}/scripts/perf-run.sh" "${arguments[@]}"
}

for ((cycle = 1; cycle <= cycles; ++cycle)); do
  cycle_dir="${output_dir}/runs/cycle-$(printf '%02d' "${cycle}")"
  run_revision base "${base_dir}" "${base_build_dir}" "${cycle_dir}/01-base"
  run_revision head "${head_dir}" "${head_build_dir}" "${cycle_dir}/02-head"
  run_revision head "${head_dir}" "${head_build_dir}" "${cycle_dir}/03-head"
  run_revision base "${base_dir}" "${base_build_dir}" "${cycle_dir}/04-base"
done

python3 "${tooling_dir}/scripts/perf-compare.py" pair-manifest \
  --runs-dir "${output_dir}/runs" \
  --cycles "${cycles}" \
  --mode "${mode}" \
  --filter "${benchmark_filter}" \
  --output "${output_dir}/manifest.json"

compare_arguments=(
  python3 "${tooling_dir}/scripts/perf-compare.py" compare
  --manifest "${output_dir}/manifest.json"
  --output-dir "${output_dir}/comparison"
)
if [[ "${allow_dirty}" == true ]]; then
  compare_arguments+=(--allow-dirty)
fi
if [[ -n "${informational_reason}" ]]; then
  compare_arguments+=(--informational-reason "${informational_reason}")
fi
"${compare_arguments[@]}"

echo "Paired profiling artifacts: ${output_dir}"
