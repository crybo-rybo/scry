from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from scripts.quality_gate import calculate_diff_coverage, compare_reports
from scripts.quality_metrics import crap_score, load_coverage


def report(branch_percent: float, **overrides: int) -> dict:
    metrics = {
        "branch_coverage": {"covered": 9, "total": 10, "percent": branch_percent},
        "crap": {"maximum": 2.0, "violations": 0},
        "complexity": {
            "maximum": 2,
            "warnings": 0,
            "long_functions": 0,
            "long_files": 0,
        },
        "unlinked_todos": 0,
    }
    for key, value in overrides.items():
        if key in metrics["complexity"]:
            metrics["complexity"][key] = value
        else:
            metrics[key] = value
    return {"metrics": metrics}


class QualityGateTests(unittest.TestCase):
    def test_crap_penalizes_complex_uncovered_code(self) -> None:
        self.assertEqual(crap_score(6, 0.0), 42.0)
        self.assertEqual(crap_score(6, 1.0), 6.0)

    def test_ratchet_rejects_coverage_and_debt_regressions(self) -> None:
        failures = compare_reports(
            report(95.0),
            report(94.0, warnings=1, long_functions=1, unlinked_todos=1),
        )
        self.assertEqual(len(failures), 4)
        self.assertTrue(any("branch coverage" in failure for failure in failures))

    def test_diff_coverage_counts_both_branch_outcomes(self) -> None:
        changes = {"src/machine.cpp": {12: "if (ready) {"}}
        head = {
            "coverage_files": {
                "src/machine.cpp": {
                    "lines": {12: True},
                    "branches": {12: [(3, 0)]},
                }
            },
            "functions": [],
        }
        result = calculate_diff_coverage(changes, head)
        self.assertEqual(result["covered"], 1)
        self.assertEqual(result["total"], 2)
        self.assertEqual(result["percent"], 50.0)

    def test_unmapped_changed_function_is_uncovered(self) -> None:
        changes = {"include/scry/new.hpp": {7: "return 42;"}}
        head = {
            "coverage_files": {},
            "functions": [
                {
                    "path": "include/scry/new.hpp",
                    "start_line": 6,
                    "end_line": 8,
                    "coverage_mapped": False,
                }
            ],
        }
        result = calculate_diff_coverage(changes, head)
        self.assertEqual(result["covered"], 0)
        self.assertEqual(result["total"], 1)
        self.assertEqual(result["uncovered"], ["include/scry/new.hpp:7"])

    def test_llvm_export_is_reduced_to_production_branch_data(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "include/scry/example.hpp"
            source.parent.mkdir(parents=True)
            source.write_text("inline bool example(bool value) { return value; }\n")
            export = {
                "data": [
                    {
                        "files": [
                            {
                                "filename": str(source),
                                "segments": [[1, 1, 2, True, True, False]],
                                "branches": [[1, 30, 1, 35, 2, 0, 0, 0, 4]],
                            }
                        ],
                        "functions": [
                            {
                                "filenames": [str(source)],
                                "regions": [[1, 1, 1, 51, 2, 0, 0, 0]],
                                "branches": [[1, 30, 1, 35, 2, 0, 0, 0, 4]],
                            }
                        ],
                    }
                ]
            }
            coverage_path = root / "coverage.json"
            coverage_path.write_text(json.dumps(export))

            result = load_coverage(coverage_path, root)

        file_report = result["files"]["include/scry/example.hpp"]
        self.assertTrue(file_report["lines"][1])
        self.assertEqual(file_report["branches"][1], [(2, 0)])
        self.assertEqual(result["functions"][0]["branch_total"], 2)
        self.assertEqual(result["functions"][0]["branch_covered"], 1)


if __name__ == "__main__":
    unittest.main()
