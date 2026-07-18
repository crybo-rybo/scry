#!/usr/bin/env python3

"""Validate reflection source decisions from gcovr's detailed JSON."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class CoverageFormatError(ValueError):
    """Raised when gcovr output cannot support a trustworthy gate."""


@dataclass(frozen=True)
class ReflectionCoverage:
    decision_covered: int
    decision_total: int
    function_covered: int
    function_total: int
    excluded_decisions: int
    uncheckable_decisions: int

    @property
    def decision_percent(self) -> float:
        return 100.0 * self.decision_covered / self.decision_total

    @property
    def function_percent(self) -> float:
        return 100.0 * self.function_covered / self.function_total


def _nonnegative_integer(value: Any, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise CoverageFormatError(f"{label} must be a non-negative integer")
    return value


def _file_record(document: dict[str, Any], expected_path: str) -> dict[str, Any]:
    files = document.get("files")
    if not isinstance(files, list):
        raise CoverageFormatError("gcovr JSON must contain a files list")
    matches = [
        record
        for record in files
        if isinstance(record, dict) and record.get("file") == expected_path
    ]
    if len(matches) != 1:
        raise CoverageFormatError(
            f"expected exactly one coverage record for {expected_path}"
        )
    return matches[0]


def _measure_decisions(lines: list[Any]) -> tuple[int, int, int, int]:
    decision_covered = 0
    decision_total = 0
    excluded_decisions = 0
    uncheckable_decisions = 0
    for line in lines:
        if not isinstance(line, dict):
            raise CoverageFormatError("each gcovr line must be an object")
        decision = line.get("gcovr/decision")
        excluded = line.get("gcovr/excluded", False)
        if not isinstance(excluded, bool):
            raise CoverageFormatError("gcovr/excluded must be a boolean")
        if decision is None:
            if excluded:
                raise CoverageFormatError(
                    "excluded gcovr lines must contain the documented decision"
                )
            continue
        if not isinstance(decision, dict) or not isinstance(
            decision.get("type"), str
        ):
            raise CoverageFormatError("each gcovr decision must have a type")
        if excluded:
            if decision["type"] != "switch":
                raise CoverageFormatError(
                    "only the documented GCC switch artifact may be excluded"
                )
            excluded_decisions += 1
            continue
        if decision["type"] == "uncheckable":
            uncheckable_decisions += 1
            continue
        if decision["type"] != "conditional":
            raise CoverageFormatError(
                f"unsupported included decision type: {decision['type']}"
            )
        true_count = _nonnegative_integer(
            decision.get("count_true"), "decision count_true"
        )
        false_count = _nonnegative_integer(
            decision.get("count_false"), "decision count_false"
        )
        decision_total += 2
        decision_covered += int(true_count > 0) + int(false_count > 0)

    if decision_total == 0:
        raise CoverageFormatError("no checkable source decisions were measured")
    if excluded_decisions != 1:
        raise CoverageFormatError(
            "expected exactly one documented GCC switch artifact exclusion"
        )
    return (
        decision_covered,
        decision_total,
        excluded_decisions,
        uncheckable_decisions,
    )


def _measure_functions(functions: list[Any]) -> tuple[int, int]:
    function_total = 0
    function_covered = 0
    for function in functions:
        if not isinstance(function, dict) or not isinstance(
            function.get("name"), str
        ):
            raise CoverageFormatError("each gcovr function must have a name")
        execution_count = _nonnegative_integer(
            function.get("execution_count"), "function execution_count"
        )
        function_total += 1
        function_covered += int(execution_count > 0)
    if function_total == 0:
        raise CoverageFormatError("no instrumented functions were measured")
    return function_covered, function_total


def load_reflection_coverage(
    coverage_path: Path, expected_path: str
) -> ReflectionCoverage:
    """Load the source decisions and functions owned by the runtime codec."""

    try:
        document = json.loads(coverage_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CoverageFormatError(f"could not read gcovr JSON: {error}") from error
    if not isinstance(document, dict):
        raise CoverageFormatError("gcovr JSON root must be an object")

    record = _file_record(document, expected_path)
    lines = record.get("lines")
    functions = record.get("functions")
    if not isinstance(lines, list) or not isinstance(functions, list):
        raise CoverageFormatError(
            "gcovr file record must contain lines and functions lists"
        )

    (
        decision_covered,
        decision_total,
        excluded_decisions,
        uncheckable_decisions,
    ) = _measure_decisions(lines)
    function_covered, function_total = _measure_functions(functions)

    return ReflectionCoverage(
        decision_covered=decision_covered,
        decision_total=decision_total,
        function_covered=function_covered,
        function_total=function_total,
        excluded_decisions=excluded_decisions,
        uncheckable_decisions=uncheckable_decisions,
    )


def gate_failures(
    coverage: ReflectionCoverage,
    minimum_decision_coverage: float,
    minimum_function_coverage: float,
) -> list[str]:
    """Return each failed reflection coverage floor."""

    failures = []
    if coverage.decision_percent + 1e-9 < minimum_decision_coverage:
        failures.append(
            "adjusted source-decision coverage is "
            f"{coverage.decision_percent:.3f}%; "
            f"minimum is {minimum_decision_coverage:.3f}%"
        )
    if coverage.function_percent + 1e-9 < minimum_function_coverage:
        failures.append(
            f"function coverage is {coverage.function_percent:.3f}%; "
            f"minimum is {minimum_function_coverage:.3f}%"
        )
    return failures


def _percentage(value: str) -> float:
    result = float(value)
    if not 0.0 <= result <= 100.0:
        raise argparse.ArgumentTypeError("coverage floor must be between 0 and 100")
    return result


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--coverage-json", type=Path, required=True)
    parser.add_argument("--path", required=True)
    parser.add_argument(
        "--minimum-decision-coverage", type=_percentage, default=95.0
    )
    parser.add_argument(
        "--minimum-function-coverage", type=_percentage, default=100.0
    )
    args = parser.parse_args(arguments)

    try:
        coverage = load_reflection_coverage(args.coverage_json, args.path)
    except CoverageFormatError as error:
        print(f"reflection coverage gate: {error}", file=sys.stderr)
        return 2

    print(
        "reflection codec adjusted source decisions "
        f"{coverage.decision_covered}/{coverage.decision_total} "
        f"({coverage.decision_percent:.3f}%); functions "
        f"{coverage.function_covered}/{coverage.function_total} "
        f"({coverage.function_percent:.3f}%); excluded GCC artifacts "
        f"{coverage.excluded_decisions}; uncheckable decisions "
        f"{coverage.uncheckable_decisions}"
    )
    failures = gate_failures(
        coverage,
        args.minimum_decision_coverage,
        args.minimum_function_coverage,
    )
    for failure in failures:
        print(f"reflection coverage gate: {failure}", file=sys.stderr)
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
