#!/usr/bin/env python3
"""Create and compare schema-versioned Scry profiling artifacts."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import platform
import random
import re
import shlex
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable, Optional


ENVIRONMENT_SCHEMA = "scry.performance.environment.v1"
SUMMARY_SCHEMA = "scry.performance.summary.v1"
COMPARISON_SCHEMA = "scry.performance.comparison.v1"
PAIR_MANIFEST_SCHEMA = "scry.performance.pair-manifest.v1"
SCENARIO_SCHEMA_VERSION = 1
GOOGLE_BENCHMARK_COMMIT = "192ef10025eb2c4cdd392bc502f0c852196baa48"
GLAZE_COMMIT = "8b60d82c66311c145c4d03be3b556b555a9cb111"
BOOTSTRAP_SAMPLES = 5_000
BOOTSTRAP_SEED = 0x5C7A


class EvidenceError(RuntimeError):
    """Raised when artifacts cannot support a valid comparison."""


def run_command(arguments: list[str], cwd: Optional[Path] = None) -> str:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise EvidenceError(f"command failed ({' '.join(arguments)}): {detail}")
    return result.stdout.strip()


def optional_command(arguments: list[str], cwd: Optional[Path] = None) -> Optional[str]:
    try:
        return run_command(arguments, cwd)
    except (EvidenceError, OSError):
        return None


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise EvidenceError(f"cannot read JSON artifact {path}: {error}") from error
    if not isinstance(value, dict):
        raise EvidenceError(f"JSON artifact is not an object: {path}")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def parse_cmake_cache(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise EvidenceError(f"cannot read configured build cache {path}: {error}") from error
    for line in lines:
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        key = key_and_type.split(":", 1)[0]
        values[key] = value
    return values


def parse_compiler_configuration(build_dir: Path) -> dict[str, str]:
    candidates = sorted(
        build_dir.glob("CMakeFiles/*/CMakeCXXCompiler.cmake"),
        key=lambda path: len(path.parts),
    )
    if not candidates:
        return {}
    text = candidates[0].read_text(encoding="utf-8")
    result: dict[str, str] = {}
    for cmake_key, output_key in (
        ("CMAKE_CXX_COMPILER_ID", "id"),
        ("CMAKE_CXX_COMPILER_VERSION", "version"),
        ("CMAKE_CXX_SIMULATE_ID", "simulate_id"),
    ):
        match = re.search(rf'set\({cmake_key} "([^"]*)"\)', text)
        if match:
            result[output_key] = match.group(1)
    return result


def standard_library(compiler: str, flags: str) -> dict[str, str]:
    arguments = [compiler]
    arguments.extend(shlex.split(flags))
    arguments.extend(["-std=c++23", "-dM", "-E", "-x", "c++", "-"])
    try:
        result = subprocess.run(
            arguments,
            input="#include <version>\n",
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError:
        return {"id": "unknown", "version": "unknown"}
    macros = result.stdout
    for macro, library_id in (
        ("_LIBCPP_VERSION", "libc++"),
        ("__GLIBCXX__", "libstdc++"),
        ("_MSVC_STL_VERSION", "msvc-stl"),
    ):
        match = re.search(rf"#define {macro} ([^\s]+)", macros)
        if match:
            return {"id": library_id, "version": match.group(1)}
    return {"id": "unknown", "version": "unknown"}


def cpu_model() -> str:
    if sys.platform == "darwin":
        value = optional_command(["sysctl", "-n", "machdep.cpu.brand_string"])
        if value:
            return value
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.lower().startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()
    return platform.processor() or "unknown"


def digest_paths(entries: Iterable[tuple[str, Path]]) -> str:
    digest = hashlib.sha256()
    for label, path in sorted(entries, key=lambda entry: entry[0]):
        encoded_label = label.encode("utf-8")
        digest.update(len(encoded_label).to_bytes(8, "big"))
        digest.update(encoded_label)
        content = path.read_bytes()
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(content)
    return digest.hexdigest()


def benchmark_source_entries(source_dir: Path) -> list[tuple[str, Path]]:
    benchmark_dir = source_dir / "benchmarks"
    return [
        (path.relative_to(source_dir).as_posix(), path)
        for path in benchmark_dir.rglob("*")
        if path.is_file()
        and path.suffix in {".cpp", ".h", ".hpp", ".cc", ".cxx", ".txt"}
    ]


def source_digest(source_dir: Path) -> str:
    return digest_paths(benchmark_source_entries(source_dir))


def methodology_digest(source_dir: Path, tooling_dir: Path) -> str:
    entries = benchmark_source_entries(source_dir)
    for relative in (
        "scripts/perf-run.sh",
        "scripts/perf-compare.py",
        "scripts/perf-pair.sh",
    ):
        path = tooling_dir / relative
        if not path.is_file():
            raise EvidenceError(f"common tooling input is missing: {path}")
        entries.append((f"tooling/{relative}", path))
    return digest_paths(entries)


def effective_compile_context(build_dir: Path, source_dir: Path) -> dict[str, Any]:
    compile_commands_path = build_dir / "compile_commands.json"
    try:
        entries = json.loads(compile_commands_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise EvidenceError(
            f"cannot read compile commands {compile_commands_path}: {error}"
        ) from error
    scoped_flag_sets: dict[str, set[tuple[str, ...]]] = {
        "scry-production": set(),
        "benchmark": set(),
    }
    for entry in entries:
        if not isinstance(entry, dict) or "file" not in entry:
            continue
        source_path = Path(entry["file"]).resolve()
        try:
            source_path.relative_to(build_dir)
            continue
        except ValueError:
            pass
        try:
            relative_source = source_path.relative_to(source_dir)
        except ValueError:
            continue
        if relative_source.parts[0] == "src":
            scope = "scry-production"
        elif relative_source.parts[0] == "benchmarks":
            scope = "benchmark"
        else:
            continue
        lexical_source_root = Path(str(entry["file"]))
        for _ in relative_source.parts:
            lexical_source_root = lexical_source_root.parent
        lexical_build_root = Path(str(entry.get("directory", build_dir)))
        if "arguments" in entry:
            tokens = [str(token) for token in entry["arguments"]]
        else:
            tokens = shlex.split(str(entry.get("command", "")))
        if tokens:
            tokens = tokens[1:]
        flags: list[str] = []
        skip_next = False
        for token in tokens:
            if skip_next:
                skip_next = False
                continue
            if token in {"-o", "-MF", "-MT", "-MQ"}:
                skip_next = True
                continue
            token_path = Path(token)
            if not token_path.is_absolute():
                token_path = Path(str(entry.get("directory", source_dir))) / token_path
            if token == "-c" or token_path.resolve() == source_path:
                continue
            normalized = token
            for spelling in {str(build_dir), str(lexical_build_root)}:
                normalized = normalized.replace(spelling, "<BUILD>")
            for spelling in {str(source_dir), str(lexical_source_root)}:
                normalized = normalized.replace(spelling, "<SOURCE>")
            flags.append(normalized)
        scoped_flag_sets[scope].add(tuple(flags))
    missing_scopes = [
        scope for scope, flag_sets in scoped_flag_sets.items() if not flag_sets
    ]
    if missing_scopes:
        raise EvidenceError(
            "compile_commands.json is missing profiling scope(s): "
            + ", ".join(missing_scopes)
        )
    normalized_scopes = {
        scope: [list(flags) for flags in sorted(flag_sets)]
        for scope, flag_sets in sorted(scoped_flag_sets.items())
    }
    encoded = json.dumps(
        normalized_scopes, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return {
        "fingerprint_sha256": hashlib.sha256(encoded).hexdigest(),
        "scopes": normalized_scopes,
    }


def host_information() -> dict[str, Any]:
    identity = {
        "node": platform.node(),
        "operating_system": platform.system(),
        "kernel_release": platform.release(),
        "architecture": platform.machine(),
        "cpu_model": cpu_model(),
        "logical_cpu_count": os.cpu_count(),
    }
    encoded = json.dumps(identity, sort_keys=True, separators=(",", ":")).encode("utf-8")
    identity["fingerprint"] = hashlib.sha256(encoded).hexdigest()
    return identity


def git_state(directory: Path) -> dict[str, Any]:
    status = run_command(
        ["git", "status", "--porcelain=v1", "--untracked-files=normal"], directory
    )
    return {
        "commit": run_command(["git", "rev-parse", "HEAD"], directory),
        "dirty": bool(status),
        "dirty_paths": status.splitlines(),
    }


def resolved_dependency(
    build_dir: Path, directory_name: str, expected_commit: str
) -> dict[str, str]:
    dependency_dir = build_dir / "_deps" / f"{directory_name}-src"
    if not dependency_dir.is_dir():
        raise EvidenceError(f"configured dependency checkout is missing: {dependency_dir}")
    resolved_commit = run_command(["git", "rev-parse", "HEAD"], dependency_dir).lower()
    if resolved_commit != expected_commit:
        raise EvidenceError(
            f"{directory_name} checkout {resolved_commit} does not match pin {expected_commit}"
        )
    return {"expected_commit": expected_commit, "resolved_commit": resolved_commit}


def make_environment(arguments: argparse.Namespace) -> None:
    source_dir = arguments.source_dir.resolve()
    tooling_dir = arguments.tooling_dir.resolve()
    build_dir = arguments.build_dir.resolve()
    if not arguments.run_id:
        raise EvidenceError("run id must not be empty")
    cache = parse_cmake_cache(build_dir / "CMakeCache.txt")
    configured_source = cache.get("CMAKE_HOME_DIRECTORY")
    if configured_source is None or Path(configured_source).resolve() != source_dir:
        raise EvidenceError("configured build tree does not belong to the requested source")
    if cache.get("SCRY_BUILD_BENCHMARKS", "").upper() not in {
        "1",
        "ON",
        "TRUE",
        "YES",
    }:
        raise EvidenceError("configured build tree does not enable profiling targets")
    compiler = cache.get("CMAKE_CXX_COMPILER", os.environ.get("CXX", "c++"))
    build_type = cache.get("CMAKE_BUILD_TYPE", "")
    build_flags = " ".join(
        part
        for part in (
            cache.get("CMAKE_CXX_FLAGS", ""),
            cache.get(f"CMAKE_CXX_FLAGS_{build_type.upper()}", ""),
        )
        if part
    )
    compiler_config = parse_compiler_configuration(build_dir)
    compile_context = effective_compile_context(build_dir, source_dir)
    scry_options = {
        key: value
        for key, value in sorted(cache.items())
        if key.startswith("SCRY_")
    }
    standard_library_flags = build_flags
    if cache.get("SCRY_USE_LIBCXX", "").upper() in {"1", "ON", "TRUE", "YES"}:
        standard_library_flags = f"{standard_library_flags} -stdlib=libc++".strip()
    source_state = git_state(source_dir)
    tooling_state = git_state(tooling_dir)
    curl_dependency = {
        "version": cache.get("PC_CURL_VERSION", "unknown"),
        "include_directory": cache.get("CURL_INCLUDE_DIR", "unknown"),
        "library_debug": cache.get("CURL_LIBRARY_DEBUG", "unknown"),
        "library_release": cache.get("CURL_LIBRARY_RELEASE", "unknown"),
    }
    environment = {
        "schema_version": ENVIRONMENT_SCHEMA,
        "scenario_contract": {
            "schema_version": SCENARIO_SCHEMA_VERSION,
            "fixture_seed": 0,
            "source_digest_sha256": source_digest(source_dir),
            "methodology_digest_sha256": methodology_digest(source_dir, tooling_dir),
        },
        "source": source_state,
        "tooling": {
            **tooling_state,
            "common_orchestrator": True,
        },
        "run": {"id": arguments.run_id},
        "measurement": {
            "run_mode": arguments.mode,
            "targets": ["scry_timing_benchmarks", "scry_allocation_benchmarks"],
            "timing_from_allocation_binary_is_authoritative": False,
            "requested_repetitions": arguments.repetitions,
            "minimum_time": arguments.minimum_time,
            "minimum_warmup_time": arguments.minimum_warmup_time,
            "execution_order": arguments.execution_order,
            "filter": arguments.filter,
            "benchmark_arguments": arguments.benchmark_argument,
        },
        "build": {
            "type": build_type,
            "generator": cache.get("CMAKE_GENERATOR", ""),
            "effective_cxx_flags": build_flags,
            "compile_commands": compile_context,
            "configure_arguments": arguments.configure_argument,
            "scry_options": scry_options,
        },
        "toolchain": {
            "compiler": compiler,
            "compiler_id": compiler_config.get("id", "unknown"),
            "compiler_version": compiler_config.get("version", "unknown"),
            "compiler_simulate_id": compiler_config.get("simulate_id", ""),
            "standard_library": standard_library(compiler, standard_library_flags),
        },
        "host": host_information(),
        "allocator": {
            "selection": "system-default",
            "libc": {
                "id": platform.libc_ver()[0] or "unknown",
                "version": platform.libc_ver()[1] or "unknown",
            },
        },
        "dependencies": {
            "google_benchmark": resolved_dependency(
                build_dir, "google_benchmark", GOOGLE_BENCHMARK_COMMIT
            ),
            "glaze": resolved_dependency(build_dir, "glaze", GLAZE_COMMIT),
            "curl": curl_dependency,
        },
    }
    write_json(arguments.output.resolve(), environment)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def median_absolute_deviation(samples: list[float]) -> float:
    center = statistics.median(samples)
    return statistics.median(abs(sample - center) for sample in samples)


def sample_statistics(samples: list[float]) -> dict[str, Any]:
    if not samples:
        return {"count": 0, "median": None, "mad": None, "normalized_mad": None}
    center = float(statistics.median(samples))
    mad = float(median_absolute_deviation(samples))
    return {
        "count": len(samples),
        "median": center,
        "mad": mad,
        "normalized_mad": mad / abs(center) if center != 0.0 else None,
        "minimum": min(samples),
        "maximum": max(samples),
    }


def time_in_nanoseconds(value: float, unit: str) -> float:
    factors = {"ns": 1.0, "us": 1_000.0, "ms": 1_000_000.0, "s": 1_000_000_000.0}
    if unit not in factors:
        raise EvidenceError(f"unsupported Google Benchmark time unit: {unit}")
    return value * factors[unit]


def benchmark_mode(raw_path: Path) -> str:
    if "allocation" in raw_path.stem:
        return "allocations"
    return "timing"


def validate_raw_context(
    raw_path: Path, context: Any, mode: str, environment: dict[str, Any]
) -> dict[str, Any]:
    if not isinstance(context, dict):
        raise EvidenceError(f"Google Benchmark context is not an object: {raw_path}")
    expected_measurement_mode = "allocation_pressure" if mode == "allocations" else "timing"
    required = {
        "scry_benchmark_schema": str(SCENARIO_SCHEMA_VERSION),
        "scry_fixture_seed": "0",
        "scry_measurement_mode": expected_measurement_mode,
        "scry_benchmark_target": raw_path.stem,
        "scry_run_id": str(nested_value(environment, "run.id")),
        "host_name": str(nested_value(environment, "host.node")),
        "num_cpus": str(nested_value(environment, "host.logical_cpu_count")),
    }
    mismatches = [
        key for key, expected in required.items() if str(context.get(key)) != expected
    ]
    if mismatches:
        raise EvidenceError(
            f"invalid Google Benchmark context in {raw_path}: {', '.join(mismatches)}"
        )
    json_schema = context.get("json_schema_version")
    if json_schema is not None:
        try:
            supported_json_schema = int(json_schema) == 1
        except (TypeError, ValueError):
            supported_json_schema = False
        if not supported_json_schema:
            raise EvidenceError(f"unsupported Google Benchmark JSON schema in {raw_path}")
    library_version = context.get("library_version")
    if library_version is not None and library_version != "v1.9.5":
        raise EvidenceError(f"unexpected Google Benchmark version in {raw_path}")
    return context


def benchmark_name(row: dict[str, Any]) -> str:
    return str(row.get("run_name") or row.get("name") or "")


def is_measurement_row(row: dict[str, Any]) -> bool:
    if row.get("error_occurred"):
        raise EvidenceError(
            f"benchmark {benchmark_name(row)} failed: {row.get('error_message', 'unknown error')}"
        )
    return row.get("run_type", "iteration") != "aggregate" and "aggregate_name" not in row


def numeric_counters(row: dict[str, Any]) -> dict[str, float]:
    reserved = {
        "aggregate_name",
        "aggregate_unit",
        "error_message",
        "error_occurred",
        "family_index",
        "iterations",
        "label",
        "name",
        "per_family_instance_index",
        "real_time",
        "repetition_index",
        "repetitions",
        "run_name",
        "run_type",
        "threads",
        "time_unit",
        "cpu_time",
    }
    return {
        key: float(value)
        for key, value in row.items()
        if key not in reserved
        and isinstance(value, (int, float))
        and not isinstance(value, bool)
    }


def summarize_raw_files(
    raw_paths: Iterable[Path], environment: dict[str, Any]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    raw_files: list[dict[str, Any]] = []
    grouped: dict[tuple[str, str], dict[str, Any]] = {}
    for raw_path in sorted(raw_paths):
        document = read_json(raw_path)
        mode = benchmark_mode(raw_path)
        expected_target = (
            "scry_allocation_benchmarks"
            if mode == "allocations"
            else "scry_timing_benchmarks"
        )
        if raw_path.stem != expected_target:
            raise EvidenceError(f"unexpected profiling target artifact: {raw_path}")
        context = validate_raw_context(
            raw_path, document.get("context", {}), mode, environment
        )
        raw_files.append(
            {
                "path": raw_path.name,
                "sha256": sha256_file(raw_path),
                "mode": mode,
                "context": context,
            }
        )
        rows = document.get("benchmarks", [])
        if not isinstance(rows, list):
            raise EvidenceError(f"Google Benchmark output has no benchmark list: {raw_path}")
        for value in rows:
            if not isinstance(value, dict) or not is_measurement_row(value):
                continue
            name = benchmark_name(value)
            if not name:
                raise EvidenceError(f"benchmark row has no name: {raw_path}")
            key = (mode, name)
            group = grouped.setdefault(
                key,
                {
                    "id": f"{mode}:{name}",
                    "name": name,
                    "family": name.split("/", 1)[0],
                    "parameters": name.split("/")[1:],
                    "scenario_version": SCENARIO_SCHEMA_VERSION,
                    "mode": mode,
                    "labels": [],
                    "samples": {"cpu_time_ns": [], "real_time_ns": []},
                    "counter_samples": {},
                },
            )
            unit = str(value.get("time_unit", "ns"))
            group["samples"]["cpu_time_ns"].append(
                time_in_nanoseconds(float(value.get("cpu_time", 0.0)), unit)
            )
            group["samples"]["real_time_ns"].append(
                time_in_nanoseconds(float(value.get("real_time", 0.0)), unit)
            )
            label = value.get("label")
            if isinstance(label, str) and label not in group["labels"]:
                group["labels"].append(label)
            for counter_name, counter_value in numeric_counters(value).items():
                group["counter_samples"].setdefault(counter_name, []).append(counter_value)

    if raw_files:
        raw_targets = {Path(raw_file["path"]).stem for raw_file in raw_files}
        expected_targets = set(nested_value(environment, "measurement.targets") or [])
        if raw_targets != expected_targets or raw_targets != {
            "scry_timing_benchmarks",
            "scry_allocation_benchmarks",
        }:
            raise EvidenceError("raw benchmark target set does not match the environment")
        shared_context_fields = (
            "json_schema_version",
            "library_version",
            "library_build_type",
            "host_name",
            "num_cpus",
            "caches",
            "scry_benchmark_schema",
            "scry_fixture_seed",
            "scry_run_id",
        )
        mixed_fields = [
            field
            for field in shared_context_fields
            if len(
                {
                    json.dumps(raw_file["context"].get(field), sort_keys=True)
                    for raw_file in raw_files
                }
            )
            > 1
        ]
        if mixed_fields:
            raise EvidenceError(
                f"raw benchmark files have mixed contexts: {', '.join(mixed_fields)}"
            )

    scenarios: list[dict[str, Any]] = []
    for key in sorted(grouped):
        group = grouped[key]
        timing_sample_count = len(group["samples"]["cpu_time_ns"])
        if timing_sample_count != len(group["samples"]["real_time_ns"]):
            raise EvidenceError(f"timing sample counts differ for {group['id']}")
        if any(
            not math.isfinite(sample)
            for samples in group["samples"].values()
            for sample in samples
        ):
            raise EvidenceError(f"non-finite timing sample for {group['id']}")
        required_counters = {
            "checksum_hi",
            "checksum_lo",
            "input_bytes",
            "items",
            "output_bytes",
        }
        if group["mode"] == "allocations":
            required_counters.update({"cpp_allocations", "cpp_requested_bytes"})
        missing_counters = sorted(required_counters - group["counter_samples"].keys())
        if missing_counters:
            raise EvidenceError(
                f"required counters missing for {group['id']}: "
                + ", ".join(missing_counters)
            )
        malformed_counters = [
            counter_name
            for counter_name, samples in group["counter_samples"].items()
            if len(samples) != timing_sample_count
            or any(not math.isfinite(sample) for sample in samples)
        ]
        if malformed_counters:
            raise EvidenceError(
                f"counter samples are incomplete or non-finite for {group['id']}: "
                + ", ".join(sorted(malformed_counters))
            )
        group["statistics"] = {
            name: sample_statistics(samples)
            for name, samples in group["samples"].items()
        }
        group["counters"] = {
            name: {
                "samples": samples,
                "statistics": sample_statistics(samples),
            }
            for name, samples in sorted(group["counter_samples"].items())
        }
        for counter_name, counter in group["counters"].items():
            samples = counter["samples"]
            if semantic_counter(counter_name) and any(
                sample != samples[0] for sample in samples[1:]
            ):
                raise EvidenceError(
                    f"semantic counter {counter_name} is not constant for {group['id']}"
                )
        del group["counter_samples"]
        scenarios.append(group)
    timing_names = {
        scenario["name"] for scenario in scenarios if scenario["mode"] == "timing"
    }
    allocation_names = {
        scenario["name"] for scenario in scenarios if scenario["mode"] == "allocations"
    }
    if timing_names != allocation_names:
        raise EvidenceError(
            "timing/allocation scenario sets differ; "
            f"timing-only={sorted(timing_names - allocation_names)}, "
            f"allocation-only={sorted(allocation_names - timing_names)}"
        )
    return raw_files, scenarios


def summarized_document(
    environment: dict[str, Any], raw_paths: Iterable[Path]
) -> dict[str, Any]:
    if environment.get("schema_version") != ENVIRONMENT_SCHEMA:
        raise EvidenceError("unsupported environment schema")
    raw_paths = list(raw_paths)
    if not raw_paths:
        raise EvidenceError("no raw Google Benchmark JSON files found")
    raw_files, scenarios = summarize_raw_files(raw_paths, environment)
    measurement = environment.get("measurement", {})
    run_mode = measurement.get("run_mode")
    if run_mode not in {"full", "smoke", "dry"}:
        raise EvidenceError("environment contains an unsupported run mode")
    if run_mode == "dry":
        if scenarios:
            raise EvidenceError("dry-run artifacts must not contain measurements")
    else:
        if not scenarios:
            raise EvidenceError("non-dry artifacts contain no benchmark measurements")
        requested_repetitions = measurement.get("requested_repetitions")
        if not isinstance(requested_repetitions, int) or requested_repetitions < 1:
            raise EvidenceError("environment contains an invalid repetition count")
        incomplete = [
            scenario["id"]
            for scenario in scenarios
            if len(scenario["samples"]["cpu_time_ns"]) != requested_repetitions
            or len(scenario["samples"]["real_time_ns"]) != requested_repetitions
        ]
        if incomplete:
            raise EvidenceError(
                "benchmark sample counts do not match the requested repetitions: "
                + ", ".join(incomplete)
            )
    return {
        "schema_version": SUMMARY_SCHEMA,
        "environment": environment,
        "raw_files": raw_files,
        "scenarios": scenarios,
    }


def make_summary(arguments: argparse.Namespace) -> None:
    environment = read_json(arguments.environment.resolve())
    raw_dir = arguments.raw_dir.resolve()
    summary = summarized_document(environment, raw_dir.glob("*.json"))
    write_json(arguments.output.resolve(), summary)


def make_empty_raw(arguments: argparse.Namespace) -> None:
    environment = read_json(arguments.environment.resolve())
    measurement_mode = (
        "allocation_pressure" if "allocation" in arguments.target else "timing"
    )
    document = {
        "context": {
            "json_schema_version": 1,
            "host_name": nested_value(environment, "host.node"),
            "num_cpus": nested_value(environment, "host.logical_cpu_count"),
            "scry_benchmark_schema": str(SCENARIO_SCHEMA_VERSION),
            "scry_fixture_seed": "0",
            "scry_measurement_mode": measurement_mode,
            "scry_benchmark_target": arguments.target,
            "scry_run_id": nested_value(environment, "run.id"),
            "scry_dry_run": True,
        },
        "benchmarks": [],
    }
    write_json(arguments.output.resolve(), document)


def artifact_summary(path: Path) -> dict[str, Any]:
    summary_path = path / "summary.json" if path.is_dir() else path
    summary = read_json(summary_path.resolve())
    if summary.get("schema_version") != SUMMARY_SCHEMA:
        raise EvidenceError(f"unsupported summary schema in {summary_path}")
    if path.is_dir():
        environment = read_json((path / "environment.json").resolve())
        if summary.get("environment") != environment:
            raise EvidenceError(
                f"standalone and summary-embedded environments differ: {path}"
            )
        recomputed = summarized_document(environment, (path / "raw").glob("*.json"))
        if summary.get("raw_files") != recomputed["raw_files"]:
            raise EvidenceError(f"cached raw-file summary does not match raw data: {path}")
        if summary.get("scenarios") != recomputed["scenarios"]:
            raise EvidenceError(f"cached scenario summary does not match raw data: {path}")
        return recomputed
    return summary


def nested_value(value: dict[str, Any], dotted_path: str) -> Any:
    current: Any = value
    for part in dotted_path.split("."):
        if not isinstance(current, dict) or part not in current:
            return None
        current = current[part]
    return current


def verify_compatible(base: dict[str, Any], head: dict[str, Any]) -> None:
    fields = (
        "environment.schema_version",
        "environment.scenario_contract.schema_version",
        "environment.scenario_contract.fixture_seed",
        "environment.scenario_contract.source_digest_sha256",
        "environment.scenario_contract.methodology_digest_sha256",
        "environment.tooling.commit",
        "environment.tooling.common_orchestrator",
        "environment.measurement.run_mode",
        "environment.measurement.requested_repetitions",
        "environment.measurement.minimum_time",
        "environment.measurement.minimum_warmup_time",
        "environment.measurement.execution_order",
        "environment.measurement.filter",
        "environment.measurement.benchmark_arguments",
        "environment.build.type",
        "environment.build.generator",
        "environment.build.effective_cxx_flags",
        "environment.build.compile_commands.fingerprint_sha256",
        "environment.build.compile_commands.scopes",
        "environment.build.configure_arguments",
        "environment.build.scry_options",
        "environment.toolchain.compiler",
        "environment.toolchain.compiler_id",
        "environment.toolchain.compiler_version",
        "environment.toolchain.standard_library",
        "environment.host.fingerprint",
        "environment.allocator",
        "environment.dependencies.google_benchmark.expected_commit",
        "environment.dependencies.google_benchmark.resolved_commit",
        "environment.dependencies.glaze.expected_commit",
        "environment.dependencies.glaze.resolved_commit",
        "environment.dependencies.curl",
    )
    missing = [
        field
        for field in fields
        if nested_value(base, field) is None or nested_value(head, field) is None
    ]
    if missing:
        raise EvidenceError(
            f"profiling contexts are missing required fields: {', '.join(missing)}"
        )
    mismatches = [
        field
        for field in fields
        if nested_value(base, field) != nested_value(head, field)
    ]
    if mismatches:
        details = ", ".join(mismatches)
        raise EvidenceError(f"incompatible profiling contexts: {details}")

    base_ids = {scenario["id"] for scenario in base.get("scenarios", [])}
    head_ids = {scenario["id"] for scenario in head.get("scenarios", [])}
    if base_ids != head_ids:
        missing = sorted(base_ids - head_ids)
        added = sorted(head_ids - base_ids)
        raise EvidenceError(
            f"incompatible scenario sets; missing from head={missing}, added in head={added}"
        )


def validate_summary_binding(summary: dict[str, Any], artifact_path: Path) -> None:
    environment = summary.get("environment", {})
    run_id = nested_value(environment, "run.id")
    if not isinstance(run_id, str) or not run_id:
        raise EvidenceError(f"artifact has no run id: {artifact_path}")
    for state_name in ("source", "tooling"):
        state = environment.get(state_name, {})
        if not isinstance(state.get("commit"), str) or not isinstance(
            state.get("dirty"), bool
        ):
            raise EvidenceError(
                f"artifact has an invalid {state_name} state: {artifact_path}"
            )
    for dependency_name, expected_commit in (
        ("google_benchmark", GOOGLE_BENCHMARK_COMMIT),
        ("glaze", GLAZE_COMMIT),
    ):
        dependency = nested_value(environment, f"dependencies.{dependency_name}")
        if not isinstance(dependency, dict) or dependency != {
            "expected_commit": expected_commit,
            "resolved_commit": expected_commit,
        }:
            raise EvidenceError(
                f"artifact dependency pin is not corroborated for {dependency_name}: "
                f"{artifact_path}"
            )
    host_name = nested_value(environment, "host.node")
    cpu_count = nested_value(environment, "host.logical_cpu_count")
    seen_modes: set[str] = set()
    for raw_file in summary.get("raw_files", []):
        context = raw_file.get("context", {})
        expected_target = Path(str(raw_file.get("path", ""))).stem
        expected = {
            "scry_run_id": run_id,
            "scry_benchmark_target": expected_target,
            "host_name": host_name,
            "num_cpus": cpu_count,
        }
        mismatches = [
            key for key, value in expected.items() if str(context.get(key)) != str(value)
        ]
        if mismatches:
            raise EvidenceError(
                f"summary/raw binding mismatch in {artifact_path}: "
                + ", ".join(mismatches)
            )
        seen_modes.add(str(raw_file.get("mode")))
        if artifact_path.is_dir():
            raw_path = artifact_path / "raw" / str(raw_file.get("path", ""))
            if not raw_path.is_file() or sha256_file(raw_path) != raw_file.get("sha256"):
                raise EvidenceError(f"raw artifact hash mismatch: {raw_path}")
    if seen_modes != {"timing", "allocations"}:
        raise EvidenceError(f"artifact does not bind both benchmark targets: {artifact_path}")


def aggregate_summaries(
    artifacts: list[tuple[Path, dict[str, Any]]], revision: str
) -> dict[str, Any]:
    if not artifacts:
        raise EvidenceError(f"no {revision} artifacts supplied")
    for path, summary in artifacts:
        validate_summary_binding(summary, path)
    aggregate = copy.deepcopy(artifacts[0][1])
    source = nested_value(aggregate, "environment.source")
    tooling = nested_value(aggregate, "environment.tooling")
    aggregate_scenarios = aggregate.get("scenarios")
    if not isinstance(aggregate_scenarios, list):
        raise EvidenceError(f"{revision} artifact has no scenario list")
    aggregate_by_id = {
        scenario.get("id"): scenario
        for scenario in aggregate_scenarios
        if isinstance(scenario, dict)
    }
    if len(aggregate_by_id) != len(aggregate_scenarios) or None in aggregate_by_id:
        raise EvidenceError(f"{revision} artifact has malformed or duplicate scenarios")
    for path, summary in artifacts[1:]:
        verify_compatible(aggregate, summary)
        if nested_value(summary, "environment.source") != source:
            raise EvidenceError(f"{revision} artifacts came from different source states")
        if nested_value(summary, "environment.tooling") != tooling:
            raise EvidenceError(f"{revision} artifacts used different common tooling states")
        for scenario in summary.get("scenarios", []):
            scenario_id = scenario.get("id")
            destination = aggregate_by_id.get(scenario_id)
            if destination is None:
                raise EvidenceError(
                    f"{revision} artifact contains an unexpected scenario: {scenario_id}"
                )
            source_samples = scenario.get("samples")
            destination_samples = destination.get("samples")
            if not isinstance(source_samples, dict) or not isinstance(
                destination_samples, dict
            ):
                raise EvidenceError(f"malformed timing samples for {scenario_id}")
            if set(source_samples) != set(destination_samples) or set(source_samples) != {
                "cpu_time_ns",
                "real_time_ns",
            }:
                raise EvidenceError(f"timing metric set mismatch for {scenario_id}")
            source_counters = scenario.get("counters")
            destination_counters = destination.get("counters")
            if not isinstance(source_counters, dict) or not isinstance(
                destination_counters, dict
            ):
                raise EvidenceError(f"malformed counters for {scenario_id}")
            if set(source_counters) != set(destination_counters):
                raise EvidenceError(
                    f"counter set mismatch across {revision} artifacts for {scenario_id}"
                )
            for metric, samples in source_samples.items():
                if not isinstance(samples, list):
                    raise EvidenceError(f"malformed {metric} samples for {scenario_id}")
                destination_samples[metric].extend(samples)
            for counter_name, counter in source_counters.items():
                samples = counter.get("samples") if isinstance(counter, dict) else None
                destination_counter = destination_counters.get(counter_name)
                if not isinstance(samples, list):
                    raise EvidenceError(
                        f"malformed {counter_name} samples for {scenario_id}"
                    )
                if not isinstance(destination_counter, dict) or not isinstance(
                    destination_counter.get("samples"), list
                ):
                    raise EvidenceError(
                        f"malformed destination {counter_name} samples for {scenario_id}"
                    )
                destination_counter["samples"].extend(samples)
        aggregate["raw_files"].extend(copy.deepcopy(summary.get("raw_files", [])))
    for scenario in aggregate_by_id.values():
        scenario["statistics"] = {
            name: sample_statistics(samples)
            for name, samples in scenario["samples"].items()
        }
        for counter_name, counter in scenario.get("counters", {}).items():
            samples = counter["samples"]
            if semantic_counter(counter_name) and any(
                sample != samples[0] for sample in samples[1:]
            ):
                raise EvidenceError(
                    f"semantic counter {counter_name} is not constant across "
                    f"{revision} artifacts for {scenario['id']}"
                )
            counter["statistics"] = sample_statistics(samples)
    aggregate["aggregation"] = {
        "artifact_count": len(artifacts),
        "run_ids": [
            nested_value(summary, "environment.run.id") for _, summary in artifacts
        ],
    }
    return aggregate


def validate_aggregated_sample_counts(
    summary: dict[str, Any], expected: int, revision: str
) -> None:
    for scenario in summary.get("scenarios", []):
        scenario_id = scenario.get("id", "unknown")
        samples = scenario.get("samples")
        counters = scenario.get("counters")
        if not isinstance(samples, dict) or set(samples) != {
            "cpu_time_ns",
            "real_time_ns",
        }:
            raise EvidenceError(f"malformed timing samples for {scenario_id}")
        if not isinstance(counters, dict):
            raise EvidenceError(f"malformed counters for {scenario_id}")
        mismatches = [
            metric
            for metric, values in samples.items()
            if not isinstance(values, list) or len(values) != expected
        ]
        mismatches.extend(
            f"counter:{name}"
            for name, counter in counters.items()
            if not isinstance(counter, dict)
            or not isinstance(counter.get("samples"), list)
            or len(counter["samples"]) != expected
        )
        if mismatches:
            raise EvidenceError(
                f"aggregated {revision} sample count mismatch for {scenario_id}: "
                + ", ".join(mismatches)
            )


def expected_pair_entries(cycles: int) -> list[tuple[int, int, str, str]]:
    result: list[tuple[int, int, str, str]] = []
    order = ("base", "head", "head", "base")
    ordinal = 0
    for cycle in range(1, cycles + 1):
        for slot, revision in enumerate(order, start=1):
            ordinal += 1
            result.append((ordinal, cycle, revision, f"{slot:02d}-{revision}"))
    return result


def make_pair_manifest(arguments: argparse.Namespace) -> None:
    runs_dir = arguments.runs_dir.resolve()
    output = arguments.output.resolve()
    entries: list[dict[str, Any]] = []
    seen_run_ids: set[str] = set()
    for ordinal, cycle, revision, directory_name in expected_pair_entries(
        arguments.cycles
    ):
        artifact = runs_dir / f"cycle-{cycle:02d}" / directory_name
        summary = artifact_summary(artifact)
        validate_summary_binding(summary, artifact)
        environment = summary["environment"]
        run_id = nested_value(environment, "run.id")
        if run_id in seen_run_ids:
            raise EvidenceError(f"duplicate run id in paired protocol: {run_id}")
        seen_run_ids.add(run_id)
        if nested_value(environment, "measurement.run_mode") != arguments.mode:
            raise EvidenceError(f"paired run mode mismatch in {artifact}")
        if nested_value(environment, "measurement.requested_repetitions") != 1:
            raise EvidenceError(f"paired runs require one repetition: {artifact}")
        if nested_value(environment, "measurement.filter") != arguments.filter:
            raise EvidenceError(f"paired run filter mismatch in {artifact}")
        entries.append(
            {
                "ordinal": ordinal,
                "cycle": cycle,
                "slot": ((ordinal - 1) % 4) + 1,
                "revision": revision,
                "artifact": os.path.relpath(artifact, output.parent),
                "run_id": run_id,
                "source_commit": nested_value(environment, "source.commit"),
                "tooling_commit": nested_value(environment, "tooling.commit"),
            }
        )
    manifest = {
        "schema_version": PAIR_MANIFEST_SCHEMA,
        "protocol": "fresh-process-a-b-b-a",
        "cycles": arguments.cycles,
        "samples_per_revision": arguments.cycles * 2,
        "expected_cycle_order": ["base", "head", "head", "base"],
        "mode": arguments.mode,
        "filter": arguments.filter,
        "entries": entries,
    }
    write_json(output, manifest)


def artifacts_from_manifest(
    manifest_path: Path,
) -> tuple[list[Path], list[Path], dict[str, Any], list[str]]:
    manifest = read_json(manifest_path)
    if manifest.get("schema_version") != PAIR_MANIFEST_SCHEMA:
        raise EvidenceError(f"unsupported paired manifest schema in {manifest_path}")
    cycles = manifest.get("cycles")
    if not isinstance(cycles, int) or cycles < 1:
        raise EvidenceError("paired manifest has an invalid cycle count")
    if manifest.get("protocol") != "fresh-process-a-b-b-a" or manifest.get(
        "expected_cycle_order"
    ) != ["base", "head", "head", "base"]:
        raise EvidenceError("paired manifest does not declare the A-B-B-A protocol")
    if manifest.get("samples_per_revision") != cycles * 2:
        raise EvidenceError("paired manifest declares an invalid sample count")
    expected = expected_pair_entries(cycles)
    entries = manifest.get("entries")
    if not isinstance(entries, list) or len(entries) != len(expected):
        raise EvidenceError("paired manifest does not contain exactly four runs per cycle")
    base_paths: list[Path] = []
    head_paths: list[Path] = []
    seen_run_ids: set[str] = set()
    common_tooling_commits: set[str] = set()
    for entry, (ordinal, cycle, revision, directory_name) in zip(entries, expected):
        if not isinstance(entry, dict):
            raise EvidenceError("paired manifest entry is not an object")
        if (
            entry.get("ordinal") != ordinal
            or entry.get("cycle") != cycle
            or entry.get("slot") != ((ordinal - 1) % 4) + 1
            or entry.get("revision") != revision
        ):
            raise EvidenceError("paired manifest order is not exact A-B-B-A")
        run_id = entry.get("run_id")
        if not isinstance(run_id, str) or not run_id or run_id in seen_run_ids:
            raise EvidenceError("paired manifest run ids are missing or duplicated")
        seen_run_ids.add(run_id)
        artifact = (manifest_path.parent / str(entry.get("artifact", ""))).resolve()
        expected_artifact = (
            manifest_path.parent
            / "runs"
            / f"cycle-{cycle:02d}"
            / directory_name
        ).resolve()
        if artifact != expected_artifact:
            raise EvidenceError("paired manifest artifact path does not match its slot")
        if not artifact.is_dir():
            raise EvidenceError(f"paired manifest artifact is not a directory: {artifact}")
        summary = artifact_summary(artifact)
        validate_summary_binding(summary, artifact)
        if nested_value(summary, "environment.tooling.common_orchestrator") is not True:
            raise EvidenceError(
                f"paired artifact does not declare common orchestration: {artifact}"
            )
        if nested_value(summary, "environment.run.id") != run_id:
            raise EvidenceError(f"paired manifest run id mismatch for {artifact}")
        if nested_value(summary, "environment.source.commit") != entry.get(
            "source_commit"
        ):
            raise EvidenceError(f"paired manifest source mismatch for {artifact}")
        tooling_commit = nested_value(summary, "environment.tooling.commit")
        if tooling_commit != entry.get("tooling_commit"):
            raise EvidenceError(f"paired manifest tooling mismatch for {artifact}")
        if revision == "head" and tooling_commit != nested_value(
            summary, "environment.source.commit"
        ):
            raise EvidenceError(
                f"common tooling is not from the HEAD artifact revision: {artifact}"
            )
        common_tooling_commits.add(str(tooling_commit))
        if nested_value(summary, "environment.measurement.run_mode") != manifest.get(
            "mode"
        ):
            raise EvidenceError(f"paired manifest mode mismatch for {artifact}")
        if nested_value(
            summary, "environment.measurement.requested_repetitions"
        ) != 1:
            raise EvidenceError(f"paired artifact must contain one repetition: {artifact}")
        if nested_value(summary, "environment.measurement.filter") != manifest.get(
            "filter", ""
        ):
            raise EvidenceError(f"paired manifest filter mismatch for {artifact}")
        validate_aggregated_sample_counts(summary, 1, f"paired artifact {ordinal}")
        (base_paths if revision == "base" else head_paths).append(artifact)
    if len(common_tooling_commits) != 1:
        raise EvidenceError("paired artifacts did not use one common tooling revision")
    expected_samples = cycles * 2
    if len(base_paths) != expected_samples or len(head_paths) != expected_samples:
        raise EvidenceError("paired manifest sample count is inconsistent with its cycles")
    reasons: list[str] = []
    if cycles != 5 or expected_samples != 10:
        reasons.append("paired protocol is not five cycles / ten samples per revision")
    if manifest.get("mode") != "full":
        reasons.append("smoke-mode characterization is not review evidence")
    protocol = {
        "manifest": str(manifest_path),
        "name": manifest.get("protocol"),
        "cycles": cycles,
        "samples_per_revision": expected_samples,
        "order_validated": True,
        "common_tooling_commit": next(iter(common_tooling_commits)),
    }
    return base_paths, head_paths, protocol, reasons


def percentile(sorted_samples: list[float], probability: float) -> float:
    if len(sorted_samples) == 1:
        return sorted_samples[0]
    position = probability * (len(sorted_samples) - 1)
    lower = int(position)
    upper = min(lower + 1, len(sorted_samples) - 1)
    fraction = position - lower
    return sorted_samples[lower] * (1.0 - fraction) + sorted_samples[upper] * fraction


def bootstrap_ratio(base: list[float], head: list[float], seed: int) -> dict[str, Any]:
    base_median = float(statistics.median(base))
    head_median = float(statistics.median(head))
    if base_median == 0.0:
        return {"ratio": None, "percent_change": None, "ci95": [None, None]}
    ratio = head_median / base_median
    if len(set(base)) == 1 and len(set(head)) == 1:
        return {
            "ratio": ratio,
            "percent_change": (ratio - 1.0) * 100.0,
            "ci95": [ratio, ratio],
        }
    generator = random.Random(seed)
    ratios: list[float] = []
    for _ in range(BOOTSTRAP_SAMPLES):
        base_sample = generator.choices(base, k=len(base))
        head_sample = generator.choices(head, k=len(head))
        resampled_base_median = float(statistics.median(base_sample))
        if resampled_base_median != 0.0:
            ratios.append(float(statistics.median(head_sample)) / resampled_base_median)
    ratios.sort()
    return {
        "ratio": ratio,
        "percent_change": (ratio - 1.0) * 100.0,
        "ci95": [percentile(ratios, 0.025), percentile(ratios, 0.975)] if ratios else [None, None],
    }


def compare_samples(base: list[float], head: list[float], seed: int) -> dict[str, Any]:
    if not base or not head:
        raise EvidenceError("comparison requires at least one sample from each revision")
    return {
        "base": sample_statistics(base),
        "head": sample_statistics(head),
        "head_over_base": bootstrap_ratio(base, head, seed),
    }


def semantic_counter(name: str) -> bool:
    return name in {"checksum_hi", "checksum_lo", "input_bytes", "output_bytes", "items"}


def primary_timing_metric(family: str) -> str:
    if family in {"Pump", "Admission", "FullTurn", "Curl", "CurlTransport"}:
        return "real_time_ns"
    return "cpu_time_ns"


def compare_scenario(
    base: dict[str, Any], head: dict[str, Any], scenario_index: int
) -> dict[str, Any]:
    if base["scenario_version"] != head["scenario_version"]:
        raise EvidenceError(f"scenario version mismatch for {base['id']}")
    result = {
        "id": base["id"],
        "name": base["name"],
        "family": base["family"],
        "parameters": base["parameters"],
        "scenario_version": base["scenario_version"],
        "mode": base["mode"],
        "timing_is_authoritative": base["mode"] == "timing",
        "primary_timing_metric": primary_timing_metric(base["family"]),
        "timing": {},
        "counters": {},
    }
    for offset, metric in enumerate(("cpu_time_ns", "real_time_ns")):
        result["timing"][metric] = compare_samples(
            base["samples"][metric],
            head["samples"][metric],
            BOOTSTRAP_SEED + scenario_index * 101 + offset,
        )
    base_counters = base.get("counters", {})
    head_counters = head.get("counters", {})
    if set(base_counters) != set(head_counters):
        raise EvidenceError(f"counter set mismatch for {base['id']}")
    for offset, name in enumerate(sorted(base_counters)):
        base_samples = base_counters[name]["samples"]
        head_samples = head_counters[name]["samples"]
        if semantic_counter(name):
            if base_samples[0] != head_samples[0]:
                raise EvidenceError(f"semantic counter {name} changed for {base['id']}")
        result["counters"][name] = compare_samples(
            base_samples,
            head_samples,
            BOOTSTRAP_SEED + scenario_index * 101 + 10 + offset,
        )
    return result


def markdown_number(value: Any, digits: int = 3) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):.{digits}f}"


def markdown_percent(value: Any) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):.2f}%"


def comparison_markdown(comparison: dict[str, Any]) -> str:
    base_source = comparison["base_source"]
    head_source = comparison["head_source"]
    lines = [
        "# Scry performance comparison",
        "",
        f"Base: `{base_source['commit']}` (dirty: `{str(base_source['dirty']).lower()}`)",
        "",
        f"Head: `{head_source['commit']}` (dirty: `{str(head_source['dirty']).lower()}`)",
        "",
    ]
    if comparison["evidence_eligible"]:
        lines.extend(
            [
                "> Protocol validation passed. Timing remains review evidence and never applies an automatic pass/fail threshold.",
                "",
            ]
        )
    else:
        reasons = "; ".join(comparison["evidence_ineligibility_reasons"])
        lines.extend(
            [
                "> [!WARNING]",
                f"> **NOT EVIDENCE-ELIGIBLE:** {reasons}.",
                "> Ratios are diagnostic only and must not support a performance claim.",
                "",
            ]
        )
    lines.extend(
        [
        "| Scenario | Mode | Primary metric | Base ns | Head ns | Change | 95% ratio interval |",
        "|---|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for scenario in comparison["scenarios"]:
        if scenario["mode"] != "timing":
            continue
        primary = scenario["timing"][scenario["primary_timing_metric"]]
        ratio = primary["head_over_base"]
        ci = ratio["ci95"]
        name = scenario["name"].replace("|", "\\|")
        lines.append(
            f"| `{name}` | {scenario['mode']} | `{scenario['primary_timing_metric']}` | "
            f"{markdown_number(primary['base']['median'])} | "
            f"{markdown_number(primary['head']['median'])} | "
            f"{markdown_percent(ratio['percent_change'])} | "
            f"{markdown_number(ci[0], 4)}–{markdown_number(ci[1], 4)} |"
        )
    allocation_scenarios = [
        scenario
        for scenario in comparison["scenarios"]
        if scenario["mode"] == "allocations"
        and "cpp_allocations" in scenario["counters"]
        and "cpp_requested_bytes" in scenario["counters"]
    ]
    if allocation_scenarios:
        lines.extend(
            [
                "",
                "## C++ allocation pressure",
                "",
                "| Scenario | Base calls | Head calls | Change | Base bytes | Head bytes | Change |",
                "|---|---:|---:|---:|---:|---:|---:|",
            ]
        )
        for scenario in allocation_scenarios:
            calls = scenario["counters"]["cpp_allocations"]
            requested = scenario["counters"]["cpp_requested_bytes"]
            name = scenario["name"].replace("|", "\\|")
            lines.append(
                f"| `{name}` | {markdown_number(calls['base']['median'])} | "
                f"{markdown_number(calls['head']['median'])} | "
                f"{markdown_percent(calls['head_over_base']['percent_change'])} | "
                f"{markdown_number(requested['base']['median'])} | "
                f"{markdown_number(requested['head']['median'])} | "
                f"{markdown_percent(requested['head_over_base']['percent_change'])} |"
            )
    lines.extend(
        [
            "",
            "Negative timing changes are faster; negative allocation changes use fewer calls or requested bytes. Allocation-executable timings are retained only in JSON and are non-authoritative.",
            "Raw samples, allocation counters, normalized MAD, and bootstrap data are retained in `comparison.json`.",
            "",
        ]
    )
    return "\n".join(lines)


def make_comparison(arguments: argparse.Namespace) -> None:
    ineligibility_reasons: list[str] = []
    if arguments.manifest is not None:
        if arguments.base or arguments.head:
            raise EvidenceError("--manifest cannot be combined with --base or --head")
        base_paths, head_paths, protocol, manifest_reasons = artifacts_from_manifest(
            arguments.manifest.resolve()
        )
        ineligibility_reasons.extend(manifest_reasons)
    else:
        if not arguments.base or not arguments.head:
            raise EvidenceError("comparison requires --manifest or both --base and --head")
        base_paths = [path.resolve() for path in arguments.base]
        head_paths = [path.resolve() for path in arguments.head]
        protocol = {
            "manifest": None,
            "name": "unverified-direct-artifacts",
            "cycles": None,
            "samples_per_revision": None,
            "order_validated": False,
            "common_tooling_commit": None,
        }
        ineligibility_reasons.append("fresh-process A-B-B-A order was not manifest-validated")
    base_artifacts = [(path, artifact_summary(path)) for path in base_paths]
    head_artifacts = [(path, artifact_summary(path)) for path in head_paths]
    base = aggregate_summaries(base_artifacts, "base")
    head = aggregate_summaries(head_artifacts, "head")
    verify_compatible(base, head)
    protocol["common_tooling_commit"] = nested_value(
        base, "environment.tooling.commit"
    )
    dirty_contexts = [
        label
        for label, dirty in (
            ("base source", nested_value(base, "environment.source.dirty")),
            ("head source", nested_value(head, "environment.source.dirty")),
            ("common tooling for base", nested_value(base, "environment.tooling.dirty")),
            ("common tooling for head", nested_value(head, "environment.tooling.dirty")),
        )
        if dirty is True
    ]
    if dirty_contexts and not arguments.allow_dirty:
        raise EvidenceError(
            "dirty profiling artifacts are rejected by default: "
            + ", ".join(dirty_contexts)
            + "; use --allow-dirty for diagnostics only"
        )
    if arguments.allow_dirty:
        ineligibility_reasons.append(
            "--allow-dirty accepted " + ", ".join(dirty_contexts)
            if dirty_contexts
            else "--allow-dirty diagnostic override was requested"
        )
    if arguments.informational_reason:
        ineligibility_reasons.append(arguments.informational_reason)
    if protocol["order_validated"]:
        expected_sample_count = protocol["samples_per_revision"]
        validate_aggregated_sample_counts(base, expected_sample_count, "base")
        validate_aggregated_sample_counts(head, expected_sample_count, "head")
    base_by_id = {scenario["id"]: scenario for scenario in base["scenarios"]}
    head_by_id = {scenario["id"]: scenario for scenario in head["scenarios"]}
    scenarios = [
        compare_scenario(base_by_id[scenario_id], head_by_id[scenario_id], index)
        for index, scenario_id in enumerate(sorted(base_by_id))
    ]
    comparison = {
        "schema_version": COMPARISON_SCHEMA,
        "timing_threshold_is_gating": False,
        "evidence_eligible": not ineligibility_reasons,
        "evidence_ineligibility_reasons": ineligibility_reasons,
        "protocol": protocol,
        "bootstrap": {
            "samples": BOOTSTRAP_SAMPLES,
            "seed": BOOTSTRAP_SEED,
            "confidence_interval": 0.95,
        },
        "compatibility": {
            "same_host": True,
            "methodology_digest_sha256": nested_value(
                base, "environment.scenario_contract.methodology_digest_sha256"
            ),
            "common_tooling_commit": nested_value(base, "environment.tooling.commit"),
        },
        "base_source": base["environment"]["source"],
        "head_source": head["environment"]["source"],
        "host": base["environment"]["host"],
        "toolchain": base["environment"]["toolchain"],
        "build": base["environment"]["build"],
        "scenarios": scenarios,
    }
    output_dir = arguments.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    write_json(output_dir / "comparison.json", comparison)
    (output_dir / "comparison.md").write_text(
        comparison_markdown(comparison), encoding="utf-8"
    )
    print(output_dir / "comparison.json")
    print(output_dir / "comparison.md")


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)

    environment = commands.add_parser("environment", help="capture build and host metadata")
    environment.add_argument("--source-dir", type=Path, required=True)
    environment.add_argument("--tooling-dir", type=Path, required=True)
    environment.add_argument("--build-dir", type=Path, required=True)
    environment.add_argument("--run-id", required=True)
    environment.add_argument("--mode", choices=("full", "smoke", "dry"), required=True)
    environment.add_argument("--repetitions", type=int, required=True)
    environment.add_argument("--minimum-time", required=True)
    environment.add_argument("--minimum-warmup-time", required=True)
    environment.add_argument("--execution-order", required=True)
    environment.add_argument("--filter", default="")
    environment.add_argument("--benchmark-argument", action="append", default=[])
    environment.add_argument("--configure-argument", action="append", default=[])
    environment.add_argument("--output", type=Path, required=True)
    environment.set_defaults(action=make_environment)

    summarize = commands.add_parser("summarize", help="summarize raw Google Benchmark JSON")
    summarize.add_argument("--environment", type=Path, required=True)
    summarize.add_argument("--raw-dir", type=Path, required=True)
    summarize.add_argument("--output", type=Path, required=True)
    summarize.set_defaults(action=make_summary)

    empty_raw = commands.add_parser(
        "empty-raw", help="write a dry-run Google Benchmark-compatible envelope"
    )
    empty_raw.add_argument("--target", required=True)
    empty_raw.add_argument("--environment", type=Path, required=True)
    empty_raw.add_argument("--output", type=Path, required=True)
    empty_raw.set_defaults(action=make_empty_raw)

    pair_manifest = commands.add_parser(
        "pair-manifest", help="write and validate an ordered A-B-B-A run manifest"
    )
    pair_manifest.add_argument("--runs-dir", type=Path, required=True)
    pair_manifest.add_argument("--cycles", type=int, required=True)
    pair_manifest.add_argument("--mode", choices=("full", "smoke"), required=True)
    pair_manifest.add_argument("--filter", default="")
    pair_manifest.add_argument("--output", type=Path, required=True)
    pair_manifest.set_defaults(action=make_pair_manifest)

    compare = commands.add_parser("compare", help="compare compatible base/head artifacts")
    compare.add_argument("--base", type=Path, action="append", default=[])
    compare.add_argument("--head", type=Path, action="append", default=[])
    compare.add_argument("--manifest", type=Path)
    compare.add_argument("--allow-dirty", action="store_true")
    compare.add_argument("--informational-reason", default="")
    compare.add_argument("--output-dir", type=Path, required=True)
    compare.set_defaults(action=make_comparison)
    return root


def main() -> int:
    arguments = parser().parse_args()
    try:
        arguments.action(arguments)
    except EvidenceError as error:
        print(f"performance evidence error: {error}", file=sys.stderr)
        return 2
    except (
        AttributeError,
        IndexError,
        KeyError,
        OSError,
        TypeError,
        ValueError,
    ) as error:
        malformed = EvidenceError(f"malformed profiling artifact: {error}")
        print(f"performance evidence error: {malformed}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
