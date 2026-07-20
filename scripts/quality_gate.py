#!/usr/bin/env python3

"""Enforce Scry's absolute coverage and CRAP quality gates."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

try:
    from scripts.quality_metrics import build_report, is_core_production_path
except ModuleNotFoundError:
    from quality_metrics import build_report, is_core_production_path


EXCLUSION_TOKENS = (
    "LCOV_EXCL",
    "GCOVR_EXCL",
    "no_profile_instrument_function",
    "coverage: ignore",
)
JUSTIFICATION_TOKEN = "SCRY-COVERAGE-JUSTIFICATION:"
COMPONENT_BRANCH_FLOORS = {
    "turn machine": ("src/machine/turn_machine.cpp",),
    "SSE parser": ("src/protocol/sse.cpp",),
    "retry classifier": ("src/core/retry.cpp",),
}
MINIMUM_COMPONENT_BRANCH_COVERAGE = 95.0
# A coarse absolute backstop against erosion in files no component floor
# lists; raised deliberately as coverage grows, never lowered silently
# (ADR 0011).
MINIMUM_TOTAL_BRANCH_COVERAGE = 88.0
MAXIMUM_CRAP = 30.0
COMPLEXITY_WARNING_CCN = 10


def _untracked_changed_lines(repository: Path) -> dict[str, dict[int, str]]:
    untracked = subprocess.run(
        [
            "git",
            "-C",
            str(repository),
            "ls-files",
            "--others",
            "--exclude-standard",
            "--",
            "include",
            "src",
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    result = {}
    for path in untracked:
        if not is_core_production_path(path):
            continue
        contents = (repository / path).read_text(encoding="utf-8").splitlines()
        result[path] = dict(enumerate(contents, start=1))
    return result


def changed_lines(base_ref: str, repository: Path) -> dict[str, dict[int, str]]:
    """Return added and modified lines between base_ref and the working tree."""

    command = [
        "git",
        "-C",
        str(repository),
        "diff",
        "--unified=0",
        "--no-ext-diff",
        "--no-color",
        base_ref,
        "--",
        "include",
        "src",
    ]
    diff = subprocess.run(
        command, check=True, capture_output=True, text=True
    ).stdout.splitlines()

    result: dict[str, dict[int, str]] = defaultdict(dict)
    current_path: str | None = None
    new_line = 0
    for line in diff:
        if line.startswith("+++ b/"):
            candidate = line[6:]
            current_path = (
                candidate if is_core_production_path(candidate) else None
            )
            continue
        if line.startswith("@@"):
            match = re.search(r"\+(\d+)(?:,\d+)?", line)
            if match is not None:
                new_line = int(match.group(1))
            continue
        if current_path is None or line.startswith("---"):
            continue
        if line.startswith("+"):
            result[current_path][new_line] = line[1:]
            new_line += 1
        elif not line.startswith("-"):
            new_line += 1

    for path, lines in _untracked_changed_lines(repository).items():
        result[path].update(lines)
    return dict(result)


def _changed_line_measurement(
    line_number: int,
    content: str,
    file_coverage: dict[str, Any],
    functions: list[dict[str, Any]],
) -> tuple[int, int]:
    branches = file_coverage["branches"].get(line_number, [])
    if branches:
        covered = sum(true > 0 for true, _ in branches)
        covered += sum(false > 0 for _, false in branches)
        return len(branches) * 2, covered

    if line_number in file_coverage["lines"]:
        return 1, int(file_coverage["lines"][line_number])

    in_unmapped_function = any(
        not function["coverage_mapped"]
        and function["start_line"] <= line_number <= function["end_line"]
        for function in functions
    )
    if in_unmapped_function and content.strip() not in {"", "{", "}", "};"}:
        return 1, 0
    return 0, 0


def _normalized_file_coverage(
    coverage_files: dict[str, dict[str, Any]], path: str
) -> dict[str, Any]:
    coverage = coverage_files.get(path, {"lines": {}, "branches": {}})
    return {
        "lines": {int(line): value for line, value in coverage["lines"].items()},
        "branches": {
            int(line): value for line, value in coverage["branches"].items()
        },
    }


def calculate_diff_coverage(
    changes: dict[str, dict[int, str]], report: dict[str, Any]
) -> dict[str, Any]:
    """Calculate branch-aware coverage for changed production lines."""

    functions_by_path: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for function in report["functions"]:
        functions_by_path[function["path"]].append(function)

    total = 0
    covered = 0
    exclusions: list[str] = []
    uncovered: list[str] = []
    for path, lines in changes.items():
        if not is_core_production_path(path):
            continue
        file_coverage = _normalized_file_coverage(report["coverage_files"], path)
        for line_number, content in lines.items():
            if any(token in content for token in EXCLUSION_TOKENS):
                if JUSTIFICATION_TOKEN not in content:
                    exclusions.append(f"{path}:{line_number}")
                continue
            line_total, line_covered = _changed_line_measurement(
                line_number, content, file_coverage, functions_by_path[path]
            )
            total += line_total
            covered += line_covered
            if line_total and line_total != line_covered:
                uncovered.append(f"{path}:{line_number}")

    percent = 100.0 if total == 0 else 100.0 * covered / total
    return {
        "covered": covered,
        "total": total,
        "percent": round(percent, 3),
        "uncovered": uncovered,
        "unjustified_exclusions": exclusions,
    }


def component_branch_coverage(
    report: dict[str, Any], paths: tuple[str, ...]
) -> dict[str, float | int]:
    """Return branch coverage aggregated across one required component."""

    covered = 0
    total = 0
    coverage_files = report.get("coverage_files", {})
    for path in paths:
        file_coverage = coverage_files.get(path)
        if file_coverage is None:
            continue
        for branches in file_coverage.get("branches", {}).values():
            total += len(branches) * 2
            covered += sum(true > 0 for true, _ in branches)
            covered += sum(false > 0 for _, false in branches)
    percent = 0.0 if total == 0 else 100.0 * covered / total
    return {
        "covered": covered,
        "total": total,
        "percent": round(percent, 3),
    }


def component_coverage_failures(report: dict[str, Any]) -> list[str]:
    """Enforce the normative branch floor on each pure critical component."""

    failures = []
    for name, paths in COMPONENT_BRANCH_FLOORS.items():
        coverage = component_branch_coverage(report, paths)
        if coverage["total"] == 0:
            failures.append(f"{name} has no measured branch coverage")
        elif (
            float(coverage["percent"]) + 1e-6
            < MINIMUM_COMPONENT_BRANCH_COVERAGE
        ):
            failures.append(
                f"{name} branch coverage is {coverage['percent']:.3f}%; "
                f"minimum is {MINIMUM_COMPONENT_BRANCH_COVERAGE:.3f}%"
            )
    return failures


def gate(
    head_report: dict[str, Any],
    diff_report: dict[str, Any],
    minimum_diff_coverage: float,
) -> list[str]:
    failures = component_coverage_failures(head_report)
    total_branch = head_report["metrics"]["branch_coverage"]
    if total_branch["percent"] + 1e-6 < MINIMUM_TOTAL_BRANCH_COVERAGE:
        failures.append(
            f"total branch coverage is {total_branch['percent']:.3f}%; "
            f"minimum is {MINIMUM_TOTAL_BRANCH_COVERAGE:.3f}%"
        )
    maximum_crap = head_report["metrics"]["crap"]["maximum"]
    if maximum_crap > MAXIMUM_CRAP:
        failures.append(
            f"maximum CRAP score is {maximum_crap:.3f}; limit is {MAXIMUM_CRAP:g}"
        )
    if diff_report["percent"] + 1e-6 < minimum_diff_coverage:
        failures.append(
            f"diff branch coverage is {diff_report['percent']:.3f}%; "
            f"minimum is {minimum_diff_coverage:.3f}%"
        )
    if diff_report["unjustified_exclusions"]:
        failures.append(
            "coverage exclusions lack an inline "
            f"{JUSTIFICATION_TOKEN} comment: "
            + ", ".join(diff_report["unjustified_exclusions"])
        )
    return failures


def _analyze_command(args: argparse.Namespace) -> int:
    report = build_report(
        Path(args.source_root), Path(args.coverage_json), Path(args.lizard_csv)
    )
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


def _gate_command(args: argparse.Namespace) -> int:
    head = json.loads(Path(args.head_report).read_text(encoding="utf-8"))
    changes = changed_lines(args.base_ref, Path(args.repository))
    diff_report = calculate_diff_coverage(changes, head)

    branch = head["metrics"]["branch_coverage"]
    print(
        f"head: branch coverage {branch['covered']}/{branch['total']} "
        f"({branch['percent']:.3f}%), "
        f"max CRAP {head['metrics']['crap']['maximum']:.3f}"
    )
    for name, paths in COMPONENT_BRANCH_FLOORS.items():
        coverage = component_branch_coverage(head, paths)
        print(
            f"{name}: branch coverage {coverage['covered']}/{coverage['total']} "
            f"({coverage['percent']:.3f}%)"
        )
    if diff_report["total"]:
        print(
            "diff: branch-aware coverage "
            f"{diff_report['covered']}/{diff_report['total']} "
            f"({diff_report['percent']:.3f}%)"
        )
    else:
        print("diff: no coverable production lines changed")

    print("top CRAP scores:")
    for function in head["top_crap"]:
        print(
            f"  {function['crap']:7.3f}  {function['path']}:"
            f"{function['start_line']}  {function['name']}"
        )

    complexity_warnings = sorted(
        (
            function
            for function in head["functions"]
            if function["ccn"] > COMPLEXITY_WARNING_CCN
        ),
        key=lambda function: (-function["ccn"], function["path"]),
    )
    print(
        f"production functions over CCN {COMPLEXITY_WARNING_CCN} "
        f"(warn-only, QA-004): {len(complexity_warnings)}"
    )
    for function in complexity_warnings:
        print(
            f"  CCN {function['ccn']:3d}  {function['path']}:"
            f"{function['start_line']}  {function['name']}"
        )

    failures = gate(head, diff_report, args.minimum_diff_coverage)
    if failures:
        print("quality gate failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        if diff_report["uncovered"]:
            print(
                "  - uncovered changed lines: "
                + ", ".join(diff_report["uncovered"]),
                file=sys.stderr,
            )
        return 1

    print("quality gate passed")
    return 0


def ctest_test_binaries(document: dict[str, Any]) -> list[tuple[str, str]]:
    """Return unique native test binaries and their CTest working directories."""

    binaries: dict[str, str] = {}
    for test in document.get("tests", []):
        command = test.get("command", [])
        if not command:
            continue
        binary = str(Path(command[0]).resolve())
        if not Path(binary).is_file():
            continue
        properties = {
            item.get("name"): item.get("value")
            for item in test.get("properties", [])
        }
        working_directory = properties.get("WORKING_DIRECTORY")
        if not isinstance(working_directory, str) or not working_directory:
            raise ValueError(f"{binary} has no CTest WORKING_DIRECTORY")
        previous = binaries.setdefault(binary, working_directory)
        if previous != working_directory:
            raise ValueError(
                f"{binary} has inconsistent CTest working directories: "
                f"{previous} and {working_directory}"
            )
    return sorted(binaries.items())


def _test_binaries_command(args: argparse.Namespace) -> int:
    document = json.loads(Path(args.ctest_json).read_text(encoding="utf-8"))
    try:
        records = ctest_test_binaries(document)
    except ValueError as error:
        print(f"invalid CTest metadata: {error}", file=sys.stderr)
        return 1
    for binary, working_directory in records:
        if any(
            separator in value
            for value in (binary, working_directory)
            for separator in "\t\r\n"
        ):
            print("CTest paths may not contain tabs or newlines", file=sys.stderr)
            return 1
        print(binary, working_directory, sep="\t")
    return 0


def _run_test_binary_command(args: argparse.Namespace) -> int:
    command = args.test_command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        print("run-test-binary requires a command", file=sys.stderr)
        return 2
    try:
        completed = subprocess.run(
            command,
            cwd=args.working_directory,
            timeout=args.timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired:
        print(
            f"{command[0]} timed out after {args.timeout_seconds:g} seconds",
            file=sys.stderr,
        )
        return 124
    return completed.returncode


def _positive_seconds(value: str) -> float:
    seconds = float(value)
    if seconds <= 0:
        raise argparse.ArgumentTypeError("timeout must be greater than zero")
    return seconds


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    analyze = subparsers.add_parser("analyze")
    analyze.add_argument("--source-root", required=True)
    analyze.add_argument("--coverage-json", required=True)
    analyze.add_argument("--lizard-csv", required=True)
    analyze.add_argument("--output", required=True)
    analyze.set_defaults(handler=_analyze_command)

    gate_parser = subparsers.add_parser("gate")
    gate_parser.add_argument("--repository", required=True)
    gate_parser.add_argument("--base-ref", required=True)
    gate_parser.add_argument("--head-report", required=True)
    gate_parser.add_argument("--minimum-diff-coverage", type=float, default=90.0)
    gate_parser.set_defaults(handler=_gate_command)

    binaries = subparsers.add_parser("test-binaries")
    binaries.add_argument("--ctest-json", required=True)
    binaries.set_defaults(handler=_test_binaries_command)

    runner = subparsers.add_parser("run-test-binary")
    runner.add_argument("--working-directory", required=True)
    runner.add_argument("--timeout-seconds", type=_positive_seconds, required=True)
    runner.add_argument("test_command", nargs=argparse.REMAINDER)
    runner.set_defaults(handler=_run_test_binary_command)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    return int(args.handler(args))


if __name__ == "__main__":
    raise SystemExit(main())
