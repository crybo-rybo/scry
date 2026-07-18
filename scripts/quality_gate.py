#!/usr/bin/env python3

"""Generate and compare Scry's coverage and complexity quality metrics."""

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
    from scripts.quality_metrics import build_report
except ModuleNotFoundError:
    from quality_metrics import build_report


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
            current_path = line[6:]
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
    for path in untracked:
        contents = (repository / path).read_text(encoding="utf-8").splitlines()
        result[path].update(enumerate(contents, start=1))
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


def compare_reports(base: dict[str, Any], head: dict[str, Any]) -> list[str]:
    """Return quality-ratchet regressions."""

    failures = []
    base_metrics = base["metrics"]
    head_metrics = head["metrics"]

    if (
        head_metrics["branch_coverage"]["percent"] + 1e-6
        < base_metrics["branch_coverage"]["percent"]
    ):
        failures.append(
            "total branch coverage regressed "
            f"({base_metrics['branch_coverage']['percent']:.3f}% -> "
            f"{head_metrics['branch_coverage']['percent']:.3f}%)"
        )

    lower_is_better = (
        ("CRAP violations", ("crap", "violations")),
        ("complexity warnings", ("complexity", "warnings")),
        ("long functions", ("complexity", "long_functions")),
        ("long files", ("complexity", "long_files")),
    )
    for label, (group, metric) in lower_is_better:
        before = base_metrics[group][metric]
        after = head_metrics[group][metric]
        if after > before:
            failures.append(f"{label} regressed ({before} -> {after})")

    if head_metrics["unlinked_todos"] > base_metrics["unlinked_todos"]:
        failures.append(
            "unlinked TODO count regressed "
            f"({base_metrics['unlinked_todos']} -> "
            f"{head_metrics['unlinked_todos']})"
        )
    return failures


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


def _print_report(report: dict[str, Any], label: str) -> None:
    metrics = report["metrics"]
    branch = metrics["branch_coverage"]
    print(
        f"{label}: branch coverage {branch['covered']}/{branch['total']} "
        f"({branch['percent']:.3f}%), max CRAP {metrics['crap']['maximum']:.3f}, "
        f"complexity warnings {metrics['complexity']['warnings']}, "
        f"long functions {metrics['complexity']['long_functions']}, "
        f"long files {metrics['complexity']['long_files']}, "
        f"unlinked TODOs {metrics['unlinked_todos']}"
    )


def gate(
    base_report: dict[str, Any],
    head_report: dict[str, Any],
    diff_report: dict[str, Any],
    minimum_diff_coverage: float,
) -> list[str]:
    failures = compare_reports(base_report, head_report)
    failures.extend(component_coverage_failures(head_report))
    maximum_crap = head_report["metrics"]["crap"]["maximum"]
    if maximum_crap > 30.0:
        failures.append(f"maximum CRAP score is {maximum_crap:.3f}; limit is 30")
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
    base = json.loads(Path(args.base_report).read_text(encoding="utf-8"))
    head = json.loads(Path(args.head_report).read_text(encoding="utf-8"))
    changes = changed_lines(args.base_ref, Path(args.repository))
    diff_report = calculate_diff_coverage(changes, head)

    _print_report(base, "base")
    _print_report(head, "head")
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

    failures = gate(base, head, diff_report, args.minimum_diff_coverage)
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


def _test_binaries_command(args: argparse.Namespace) -> int:
    document = json.loads(Path(args.ctest_json).read_text(encoding="utf-8"))
    seen = set()
    for test in document.get("tests", []):
        command = test.get("command", [])
        if not command:
            continue
        binary = str(Path(command[0]).resolve())
        if binary not in seen and Path(binary).is_file():
            seen.add(binary)
            print(binary)
    return 0


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
    gate_parser.add_argument("--base-report", required=True)
    gate_parser.add_argument("--head-report", required=True)
    gate_parser.add_argument("--minimum-diff-coverage", type=float, default=90.0)
    gate_parser.set_defaults(handler=_gate_command)

    binaries = subparsers.add_parser("test-binaries")
    binaries.add_argument("--ctest-json", required=True)
    binaries.set_defaults(handler=_test_binaries_command)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    return int(args.handler(args))


if __name__ == "__main__":
    raise SystemExit(main())
