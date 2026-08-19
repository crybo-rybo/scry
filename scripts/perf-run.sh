#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
Usage: scripts/perf-run.sh [options]

Configure, build, and run Scry's opt-in profiling executables. This script may
be invoked from a common tooling checkout against another Scry source tree.

Options:
  --mode MODE           full (default), smoke, or dry
  --source-dir DIR      Scry source checkout (default: tooling checkout)
  --output DIR          artifact directory (default: timestamped under build/)
  --build-dir DIR       CMake build directory (default: build/profile)
  --filter REGEX        Google Benchmark filter (full/smoke only)
  --repetitions COUNT   override repetitions (default: full=10, smoke=1)
  --min-time DURATION   override Google Benchmark minimum time
  --cmake-arg ARG       append one configure argument; may be repeated
  --benchmark-arg ARG   append one benchmark argument; may be repeated
  --skip-configure      use an already-configured build tree
  --skip-build          use already-built profiling executables
  --help                show this help

Dry mode configures and builds both executables, records their benchmark
identities, and emits empty but valid raw/summary artifacts without executing
a workload. Timing from the allocation executable is never authoritative.
USAGE
}

mode="full"
source_dir="${root_dir}"
output_dir=""
build_dir=""
benchmark_filter=""
repetitions=""
minimum_time=""
declare -a cmake_arguments=()
declare -a benchmark_arguments=()
minimum_warmup_time="0"
execution_order="timing-then-allocation; google-benchmark-random-interleaving-within-target"
skip_configure=false
skip_build=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      [[ $# -ge 2 ]] || { echo "--mode requires a value" >&2; exit 2; }
      mode="$2"
      shift 2
      ;;
    --source-dir)
      [[ $# -ge 2 ]] || { echo "--source-dir requires a value" >&2; exit 2; }
      source_dir="$2"
      shift 2
      ;;
    --output)
      [[ $# -ge 2 ]] || { echo "--output requires a value" >&2; exit 2; }
      output_dir="$2"
      shift 2
      ;;
    --build-dir)
      [[ $# -ge 2 ]] || { echo "--build-dir requires a value" >&2; exit 2; }
      build_dir="$2"
      shift 2
      ;;
    --filter)
      [[ $# -ge 2 ]] || { echo "--filter requires a value" >&2; exit 2; }
      benchmark_filter="$2"
      shift 2
      ;;
    --repetitions)
      [[ $# -ge 2 ]] || { echo "--repetitions requires a value" >&2; exit 2; }
      repetitions="$2"
      shift 2
      ;;
    --min-time)
      [[ $# -ge 2 ]] || { echo "--min-time requires a value" >&2; exit 2; }
      minimum_time="$2"
      shift 2
      ;;
    --cmake-arg)
      [[ $# -ge 2 ]] || { echo "--cmake-arg requires a value" >&2; exit 2; }
      cmake_arguments+=("$2")
      shift 2
      ;;
    --benchmark-arg)
      [[ $# -ge 2 ]] || { echo "--benchmark-arg requires a value" >&2; exit 2; }
      benchmark_arguments+=("$2")
      shift 2
      ;;
    --skip-configure)
      skip_configure=true
      shift
      ;;
    --skip-build)
      skip_build=true
      shift
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

source_dir="$(cd "${source_dir}" && pwd)"
if [[ -z "${build_dir}" ]]; then
  build_dir="${source_dir}/build/profile"
fi

case "${mode}" in
  full)
    repetitions="${repetitions:-10}"
    minimum_time="${minimum_time:-0.25s}"
    ;;
  smoke)
    repetitions="${repetitions:-1}"
    minimum_time="${minimum_time:-0.001s}"
    ;;
  dry)
    if [[ -n "${benchmark_filter}" || -n "${repetitions}" || -n "${minimum_time}" ]]; then
      echo "dry mode does not accept filter, repetition, or minimum-time overrides" >&2
      exit 2
    fi
    repetitions="0"
    minimum_time="0s"
    execution_order="not-executed"
    ;;
  *)
    echo "Unsupported mode: ${mode}; expected full, smoke, or dry" >&2
    exit 2
    ;;
esac

if [[ "${mode}" != "dry" && ! "${repetitions}" =~ ^[1-9][0-9]*$ ]]; then
  echo "--repetitions must be a positive integer" >&2
  exit 2
fi

is_reserved_benchmark_argument() {
  case "$1" in
    --benchmark_out|--benchmark_out=*|\
    --benchmark_out_format|--benchmark_out_format=*|\
    --benchmark_repetitions|--benchmark_repetitions=*|\
    --benchmark_min_time|--benchmark_min_time=*|\
    --benchmark_min_warmup_time|--benchmark_min_warmup_time=*|\
    --benchmark_filter|--benchmark_filter=*|\
    --benchmark_report_aggregates_only|--benchmark_report_aggregates_only=*|\
    --benchmark_display_aggregates_only|--benchmark_display_aggregates_only=*|\
    --benchmark_enable_random_interleaving|--benchmark_enable_random_interleaving=*|\
    --benchmark_list_tests|--benchmark_list_tests=*|\
    --benchmark_context|--benchmark_context=*|\
    --benchmark_dry_run|--benchmark_dry_run=*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

if (( ${#benchmark_arguments[@]} > 0 )); then
  for argument in "${benchmark_arguments[@]}"; do
    if is_reserved_benchmark_argument "${argument}"; then
      echo "--benchmark-arg may not override runner-controlled flag: ${argument}" >&2
      exit 2
    fi
  done
fi

if [[ "${build_dir}" != /* ]]; then
  build_dir="${source_dir}/${build_dir}"
fi

if [[ -z "${output_dir}" ]]; then
  readonly timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
  readonly short_commit="$(git -C "${source_dir}" rev-parse --short=12 HEAD)"
  output_dir="${source_dir}/build/profile-artifacts/${timestamp}-${short_commit}"
elif [[ "${output_dir}" != /* ]]; then
  output_dir="${source_dir}/${output_dir}"
fi

if [[ -d "${output_dir}" ]]; then
  existing_artifact="$(find "${output_dir}" -type f -print -quit)"
  if [[ -n "${existing_artifact}" ]]; then
    echo "Refusing to overwrite non-empty artifact directory: ${output_dir}" >&2
    exit 2
  fi
fi

if [[ "${skip_configure}" == false ]]; then
  if (( ${#cmake_arguments[@]} > 0 )); then
    (cd "${source_dir}" && cmake --preset profile -B "${build_dir}" "${cmake_arguments[@]}")
  else
    (cd "${source_dir}" && cmake --preset profile -B "${build_dir}")
  fi
fi
if [[ "${skip_build}" == false ]]; then
  cmake --build "${build_dir}" \
    --target scry_timing_benchmarks scry_allocation_benchmarks
fi

mkdir -p "${output_dir}/raw" "${output_dir}/logs" "${output_dir}/benchmarks"
readonly run_id="$(python3 -c 'import uuid; print(uuid.uuid4())')"

environment_arguments=(
  python3 "${root_dir}/scripts/perf-compare.py" environment
  --source-dir "${source_dir}"
  --tooling-dir "${root_dir}"
  --build-dir "${build_dir}"
  --run-id "${run_id}"
  --mode "${mode}"
  --repetitions "${repetitions}"
  --minimum-time "${minimum_time}"
  --minimum-warmup-time "${minimum_warmup_time}"
  --execution-order "${execution_order}"
  --filter "${benchmark_filter}"
  --output "${output_dir}/environment.json"
)
if (( ${#cmake_arguments[@]} > 0 )); then
  for argument in "${cmake_arguments[@]}"; do
    environment_arguments+=(--configure-argument "${argument}")
  done
fi
if (( ${#benchmark_arguments[@]} > 0 )); then
  for argument in "${benchmark_arguments[@]}"; do
    environment_arguments+=(--benchmark-argument "${argument}")
  done
fi
"${environment_arguments[@]}"

find_executable() {
  local target="$1"
  local executable=""
  while IFS= read -r candidate; do
    if [[ -x "${candidate}" ]]; then
      executable="${candidate}"
      break
    fi
  done < <(find "${build_dir}" -type f -name "${target}" -print)
  if [[ -z "${executable}" ]]; then
    echo "Could not find built profiling executable: ${target}" >&2
    return 1
  fi
  printf '%s\n' "${executable}"
}

run_target() {
  local target="$1"
  local executable="$2"
  local raw_path="${output_dir}/raw/${target}.json"
  local log_path="${output_dir}/logs/${target}.log"
  local list_path="${output_dir}/benchmarks/${target}.txt"
  local -a common_arguments=(
    "--benchmark_out=${raw_path}"
    "--benchmark_out_format=json"
  )

  SCRY_BENCHMARK_RUN_ID="${run_id}" \
    "${executable}" --benchmark_list_tests=true >"${list_path}"
  if [[ ! -s "${list_path}" ]]; then
    echo "Profiling executable registered no scenarios: ${target}" >&2
    return 1
  fi

  if [[ "${mode}" == "dry" ]]; then
    python3 "${root_dir}/scripts/perf-compare.py" empty-raw \
      --target "${target}" \
      --environment "${output_dir}/environment.json" \
      --output "${raw_path}"
    printf 'Dry run: listed scenarios without executing workloads.\n' >"${log_path}"
    return
  fi

  local -a run_arguments=(
    "${common_arguments[@]}"
    "--benchmark_repetitions=${repetitions}"
    "--benchmark_min_time=${minimum_time}"
    --benchmark_report_aggregates_only=false
    --benchmark_enable_random_interleaving=true
  )
  if [[ "${mode}" == "full" ]]; then
    run_arguments+=("--benchmark_min_warmup_time=${minimum_warmup_time}")
  fi
  if [[ -n "${benchmark_filter}" ]]; then
    run_arguments+=("--benchmark_filter=${benchmark_filter}")
  fi
  if (( ${#benchmark_arguments[@]} > 0 )); then
    run_arguments+=("${benchmark_arguments[@]}")
  fi
  SCRY_BENCHMARK_RUN_ID="${run_id}" \
    "${executable}" "${run_arguments[@]}" 2>&1 | tee "${log_path}"
}

readonly timing_executable="$(find_executable scry_timing_benchmarks)"
readonly allocation_executable="$(find_executable scry_allocation_benchmarks)"
run_target scry_timing_benchmarks "${timing_executable}"
run_target scry_allocation_benchmarks "${allocation_executable}"

python3 "${root_dir}/scripts/perf-compare.py" summarize \
  --environment "${output_dir}/environment.json" \
  --raw-dir "${output_dir}/raw" \
  --output "${output_dir}/summary.json"

echo "Profiling artifacts: ${output_dir}"
