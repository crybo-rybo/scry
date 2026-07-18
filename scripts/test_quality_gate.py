from __future__ import annotations

import io
import json
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from scripts.quality_gate import (
    _run_test_binary_command,
    calculate_diff_coverage,
    compare_reports,
    component_branch_coverage,
    component_coverage_failures,
    ctest_test_binaries,
)
from scripts.quality_metrics import (
    crap_score,
    is_core_production_path,
    is_reflection_component_path,
    load_coverage,
)


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
    def test_ctest_binaries_preserve_working_directory_and_deduplicate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first"
            second = root / "second"
            first.touch()
            second.touch()
            document = {
                "tests": [
                    {
                        "command": [str(second), "case two"],
                        "properties": [
                            {"name": "WORKING_DIRECTORY", "value": "/work/two"}
                        ],
                    },
                    {
                        "command": [str(first), "case one"],
                        "properties": [
                            {"name": "WORKING_DIRECTORY", "value": "/work/one"}
                        ],
                    },
                    {
                        "command": [str(first), "another case"],
                        "properties": [
                            {"name": "WORKING_DIRECTORY", "value": "/work/one"}
                        ],
                    },
                ]
            }

            self.assertEqual(
                ctest_test_binaries(document),
                [
                    (str(first.resolve()), "/work/one"),
                    (str(second.resolve()), "/work/two"),
                ],
            )

    def test_ctest_binary_rejects_inconsistent_working_directories(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "test"
            binary.touch()
            document = {
                "tests": [
                    {
                        "command": [str(binary), "first"],
                        "properties": [
                            {"name": "WORKING_DIRECTORY", "value": "/work/one"}
                        ],
                    },
                    {
                        "command": [str(binary), "second"],
                        "properties": [
                            {"name": "WORKING_DIRECTORY", "value": "/work/two"}
                        ],
                    },
                ]
            }

            with self.assertRaisesRegex(ValueError, "inconsistent"):
                ctest_test_binaries(document)

    @patch("scripts.quality_gate.subprocess.run")
    def test_direct_test_runner_applies_working_directory_and_timeout(
        self, run: unittest.mock.Mock
    ) -> None:
        run.return_value.returncode = 0
        arguments = SimpleNamespace(
            test_command=["--", "/tmp/test", "--order", "lex"],
            working_directory="/tmp/work",
            timeout_seconds=300.0,
        )

        self.assertEqual(_run_test_binary_command(arguments), 0)
        run.assert_called_once_with(
            ["/tmp/test", "--order", "lex"],
            cwd="/tmp/work",
            timeout=300.0,
            check=False,
        )

    @patch("scripts.quality_gate.subprocess.run")
    def test_direct_test_runner_reports_timeout(
        self, run: unittest.mock.Mock
    ) -> None:
        run.side_effect = subprocess.TimeoutExpired(["/tmp/test"], 300.0)
        arguments = SimpleNamespace(
            test_command=["/tmp/test"],
            working_directory="/tmp/work",
            timeout_seconds=300.0,
        )
        error = io.StringIO()

        with redirect_stderr(error):
            result = _run_test_binary_command(arguments)

        self.assertEqual(result, 124)
        self.assertIn("timed out after 300 seconds", error.getvalue())

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

    def test_core_quality_domain_excludes_only_reflection_component(self) -> None:
        self.assertTrue(is_core_production_path("include/scry/harness.hpp"))
        self.assertTrue(is_core_production_path("src/core/harness.cpp"))
        self.assertFalse(is_core_production_path("include/scry/reflection.hpp"))
        self.assertFalse(
            is_core_production_path("include/scry/detail/reflection_codec.hpp")
        )
        self.assertFalse(is_core_production_path("src/reflection/json_bridge.cpp"))
        self.assertTrue(
            is_reflection_component_path("tests/reflection/reflection_tests.cpp")
        )
        self.assertTrue(
            is_reflection_component_path("examples/reflection_tools.cpp")
        )
        self.assertFalse(is_reflection_component_path("tests/core/example.cpp"))
        self.assertTrue(is_core_production_path("include/scry/reflections.hpp"))
        self.assertTrue(
            is_core_production_path("include/scry/detail/reflectionary.hpp")
        )
        self.assertTrue(is_core_production_path("src/reflections/json_bridge.cpp"))

    def test_core_diff_coverage_ignores_reflection_component(self) -> None:
        changes = {
            "include/scry/reflection.hpp": {7: "return 42;"},
            "include/scry/detail/reflection_codec.hpp": {8: "return 43;"},
            "src/reflection/json_bridge.cpp": {9: "return 44;"},
        }
        head = {"coverage_files": {}, "functions": []}

        result = calculate_diff_coverage(changes, head)

        self.assertEqual(result["covered"], 0)
        self.assertEqual(result["total"], 0)
        self.assertEqual(result["percent"], 100.0)

    def test_component_coverage_enforces_each_required_floor(self) -> None:
        head = {
            "coverage_files": {
                "src/machine/turn_machine.cpp": {
                    "branches": {12: [(3, 1)]},
                },
                "src/protocol/sse.cpp": {
                    "branches": {8: [(2, 1)]},
                },
                "src/core/retry.cpp": {
                    "branches": {4: [(1, 0)]},
                },
            }
        }
        coverage = component_branch_coverage(
            head, ("src/machine/turn_machine.cpp",)
        )
        self.assertEqual(coverage["percent"], 100.0)
        failures = component_coverage_failures(head)
        self.assertEqual(len(failures), 1)
        self.assertIn("retry classifier", failures[0])

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
                            },
                            {
                                "filename": str(
                                    root
                                    / "include/scry/detail/reflection_codec.hpp"
                                ),
                                "segments": [[1, 1, 0, True, True, False]],
                                "branches": [[1, 1, 1, 2, 0, 0, 0, 0, 4]],
                            },
                        ],
                        "functions": [
                            {
                                "filenames": [str(source)],
                                "regions": [[1, 1, 1, 51, 2, 0, 0, 0]],
                                "branches": [[1, 30, 1, 35, 2, 0, 0, 0, 4]],
                            },
                            {
                                "filenames": [
                                    str(
                                        root
                                        / "include/scry/detail/reflection_codec.hpp"
                                    )
                                ],
                                "regions": [[1, 1, 1, 2, 0, 0, 0, 0]],
                                "branches": [[1, 1, 1, 2, 0, 0, 0, 0, 4]],
                            },
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
        self.assertNotIn(
            "include/scry/detail/reflection_codec.hpp", result["files"]
        )
        self.assertEqual(len(result["functions"]), 1)


if __name__ == "__main__":
    unittest.main()
