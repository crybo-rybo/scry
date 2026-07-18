"""Coverage and complexity metric extraction for Scry's quality gate."""

from __future__ import annotations

import csv
import json
import re
from collections import defaultdict
from pathlib import Path
from typing import Any


PRODUCTION_PREFIXES = ("include/", "src/")
CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx", ".h"}


def _is_production(path: str) -> bool:
    return path.startswith(PRODUCTION_PREFIXES)


def _relative_path(filename: str, source_root: Path) -> str | None:
    try:
        return Path(filename).resolve().relative_to(source_root.resolve()).as_posix()
    except ValueError:
        return None


def _load_file_record(
    record: dict[str, Any], source_root: Path
) -> tuple[str, dict[str, Any]] | None:
    path = _relative_path(record["filename"], source_root)
    if path is None or not _is_production(path):
        return None

    lines: dict[int, bool] = defaultdict(bool)
    for segment in record.get("segments", []):
        line, _, count, has_count, _, is_gap = segment[:6]
        if has_count and not is_gap:
            lines[int(line)] = lines[int(line)] or int(count) > 0

    branches: dict[int, list[tuple[int, int]]] = defaultdict(list)
    for branch in record.get("branches", []):
        line, _, _, _, true_count, false_count = branch[:6]
        branches[int(line)].append((int(true_count), int(false_count)))
    return path, {"lines": dict(lines), "branches": dict(branches)}


def _branch_counts(branches: list[list[Any]]) -> tuple[int, int]:
    total = len(branches) * 2
    covered = sum(int(branch[4]) > 0 for branch in branches)
    covered += sum(int(branch[5]) > 0 for branch in branches)
    return total, covered


def _load_function_records(
    function: dict[str, Any], source_root: Path
) -> list[dict[str, Any]]:
    filenames = function.get("filenames", [])
    regions_by_file: dict[int, list[list[Any]]] = defaultdict(list)
    for region in function.get("regions", []):
        if len(region) >= 6:
            regions_by_file[int(region[5])].append(region)

    branches_by_file: dict[int, list[list[Any]]] = defaultdict(list)
    for branch in function.get("branches", []):
        if len(branch) >= 7:
            branches_by_file[int(branch[6])].append(branch)

    result = []
    for file_id, regions in regions_by_file.items():
        if file_id >= len(filenames):
            continue
        path = _relative_path(filenames[file_id], source_root)
        if path is None or not _is_production(path):
            continue
        branch_total, branch_covered = _branch_counts(
            branches_by_file.get(file_id, [])
        )
        result.append(
            {
                "path": path,
                "start_line": min(int(region[0]) for region in regions),
                "end_line": max(int(region[2]) for region in regions),
                "branch_total": branch_total,
                "branch_covered": branch_covered,
                "executed": any(int(region[4]) > 0 for region in regions),
            }
        )
    return result


def load_coverage(coverage_path: Path, source_root: Path) -> dict[str, Any]:
    """Load the llvm-cov export fields used by Scry's gates."""

    document = json.loads(coverage_path.read_text(encoding="utf-8"))
    files: dict[str, dict[str, Any]] = {}
    functions: list[dict[str, Any]] = []
    for dataset in document.get("data", []):
        for file_record in dataset.get("files", []):
            loaded = _load_file_record(file_record, source_root)
            if loaded is not None:
                path, report = loaded
                files[path] = report
        for function in dataset.get("functions", []):
            functions.extend(_load_function_records(function, source_root))
    return {"files": files, "functions": functions}


def load_lizard(lizard_path: Path) -> list[dict[str, Any]]:
    """Read lizard's stable CSV output."""

    functions: list[dict[str, Any]] = []
    with lizard_path.open(newline="", encoding="utf-8") as stream:
        for row in csv.reader(stream):
            if len(row) != 11:
                continue
            functions.append(
                {
                    "nloc": int(row[0]),
                    "ccn": int(row[1]),
                    "parameter_count": int(row[3]),
                    "length": int(row[4]),
                    "path": Path(row[6]).as_posix(),
                    "name": row[8],
                    "start_line": int(row[9]),
                    "end_line": int(row[10]),
                }
            )
    return functions


def _function_coverage(
    function: dict[str, Any], coverage_functions: list[dict[str, Any]]
) -> tuple[float, bool]:
    instances = [
        candidate
        for candidate in coverage_functions
        if candidate["path"] == function["path"]
        and function["start_line"]
        <= candidate["start_line"]
        <= function["end_line"]
    ]
    if not instances:
        return 0.0, False

    ratios = []
    for instance in instances:
        if instance["branch_total"]:
            ratios.append(instance["branch_covered"] / instance["branch_total"])
        else:
            ratios.append(1.0 if instance["executed"] else 0.0)
    return min(ratios), True


def crap_score(ccn: int, coverage: float) -> float:
    """Return the CRAP score for one function."""

    return (ccn**2) * ((1.0 - coverage) ** 3) + ccn


def _source_files(source_root: Path) -> list[Path]:
    roots = ("include", "src", "examples", "spikes", "tests")
    return sorted(
        path
        for root_name in roots
        for path in (source_root / root_name).rglob("*")
        if path.is_file() and path.suffix in CPP_SUFFIXES
    )


def _build_function_reports(
    functions: list[dict[str, Any]], coverage_functions: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    reports = []
    for function in functions:
        ratio, mapped = _function_coverage(function, coverage_functions)
        reports.append(
            {
                **function,
                "coverage": round(ratio, 6),
                "coverage_mapped": mapped,
                "crap": round(crap_score(function["ccn"], ratio), 3),
            }
        )
    return reports


def _branch_totals(files: dict[str, dict[str, Any]]) -> tuple[int, int]:
    branch_total = 0
    branch_covered = 0
    for file_report in files.values():
        for branches in file_report["branches"].values():
            branch_total += len(branches) * 2
            branch_covered += sum(true > 0 for true, _ in branches)
            branch_covered += sum(false > 0 for _, false in branches)
    return branch_total, branch_covered


def _unlinked_todo_count(source_files: list[Path]) -> int:
    unlinked_todos = 0
    for path in source_files:
        for line in path.read_text(encoding="utf-8").splitlines():
            if re.search(r"//\s*TODO\b", line) and not re.search(
                r"(https?://|#[0-9]+)", line
            ):
                unlinked_todos += 1
    return unlinked_todos


def _complexity_metrics(
    functions: list[dict[str, Any]], source_files: list[Path]
) -> dict[str, int]:
    return {
        "maximum": max((function["ccn"] for function in functions), default=0),
        "warnings": sum(function["ccn"] > 10 for function in functions),
        "long_functions": sum(function["length"] > 60 for function in functions),
        "long_files": sum(
            len(path.read_text(encoding="utf-8").splitlines()) > 500
            for path in source_files
        ),
    }


def build_report(
    source_root: Path, coverage_path: Path, lizard_path: Path
) -> dict[str, Any]:
    """Build the machine-readable quality report."""

    coverage = load_coverage(coverage_path, source_root)
    lizard_functions = load_lizard(lizard_path)
    production_functions = [
        function for function in lizard_functions if _is_production(function["path"])
    ]
    function_reports = _build_function_reports(
        production_functions, coverage["functions"]
    )
    branch_total, branch_covered = _branch_totals(coverage["files"])
    source_files = _source_files(source_root)
    top_crap = sorted(
        function_reports,
        key=lambda function: (-function["crap"], function["path"], function["start_line"]),
    )[:10]
    branch_percent = (
        100.0 if branch_total == 0 else 100.0 * branch_covered / branch_total
    )

    return {
        "schema": 1,
        "metrics": {
            "branch_coverage": {
                "covered": branch_covered,
                "total": branch_total,
                "percent": round(branch_percent, 3),
            },
            "crap": {
                "maximum": max(
                    (function["crap"] for function in function_reports), default=0.0
                ),
                "violations": sum(
                    function["crap"] > 30.0 for function in function_reports
                ),
            },
            "complexity": _complexity_metrics(lizard_functions, source_files),
            "unlinked_todos": _unlinked_todo_count(source_files),
        },
        "functions": function_reports,
        "top_crap": top_crap,
        "coverage_files": coverage["files"],
    }
