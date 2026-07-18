from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from scripts.reflection_coverage_gate import (
    CoverageFormatError,
    gate_failures,
    load_reflection_coverage,
)


CODEC_PATH = "include/scry/detail/reflection_codec.hpp"


def write_coverage(directory: str, file_record: dict) -> Path:
    path = Path(directory) / "coverage.json"
    path.write_text(json.dumps({"files": [file_record]}), encoding="utf-8")
    return path


def function(name: str = "codec", execution_count: int = 1) -> dict:
    return {"name": name, "execution_count": execution_count}


class ReflectionCoverageGateTests(unittest.TestCase):
    def test_included_conditional_counts_each_source_outcome(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = write_coverage(
                directory,
                {
                    "file": CODEC_PATH,
                    "lines": [
                        {
                            "line_number": 10,
                            "gcovr/decision": {
                                "type": "conditional",
                                "count_true": 4,
                                "count_false": 2,
                            },
                        },
                        {
                            "line_number": 11,
                            "gcovr/excluded": True,
                            "gcovr/decision": {"type": "switch", "count": 6},
                        },
                    ],
                    "functions": [function()],
                },
            )

            coverage = load_reflection_coverage(path, CODEC_PATH)

        self.assertEqual(coverage.decision_covered, 2)
        self.assertEqual(coverage.decision_total, 2)
        self.assertEqual(coverage.function_covered, 1)

    def test_explicit_switch_artifact_is_the_only_excluded_decision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = write_coverage(
                directory,
                {
                    "file": CODEC_PATH,
                    "lines": [
                        {
                            "line_number": 10,
                            "gcovr/decision": {
                                "type": "conditional",
                                "count_true": 1,
                                "count_false": 1,
                            },
                        },
                        {
                            "line_number": 11,
                            "gcovr/excluded": True,
                            "gcovr/decision": {"type": "switch", "count": 3},
                        },
                        {
                            "line_number": 12,
                            "gcovr/decision": {"type": "uncheckable"},
                        },
                    ],
                    "functions": [function()],
                },
            )

            coverage = load_reflection_coverage(path, CODEC_PATH)

        self.assertEqual(coverage.excluded_decisions, 1)
        self.assertEqual(coverage.uncheckable_decisions, 1)

    def test_malformed_or_widened_exclusions_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = write_coverage(
                directory,
                {
                    "file": CODEC_PATH,
                    "lines": [
                        {
                            "line_number": 10,
                            "gcovr/excluded": True,
                            "gcovr/decision": {
                                "type": "conditional",
                                "count_true": 1,
                                "count_false": 1,
                            },
                        }
                    ],
                    "functions": [function()],
                },
            )

            with self.assertRaises(CoverageFormatError):
                load_reflection_coverage(path, CODEC_PATH)

    def test_a_second_excluded_switch_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = write_coverage(
                directory,
                {
                    "file": CODEC_PATH,
                    "lines": [
                        {
                            "line_number": 10,
                            "gcovr/decision": {
                                "type": "conditional",
                                "count_true": 1,
                                "count_false": 1,
                            },
                        },
                        {
                            "line_number": 11,
                            "gcovr/excluded": True,
                            "gcovr/decision": {"type": "switch", "count": 2},
                        },
                        {
                            "line_number": 12,
                            "gcovr/excluded": True,
                            "gcovr/decision": {"type": "switch", "count": 3},
                        },
                    ],
                    "functions": [function()],
                },
            )

            with self.assertRaises(CoverageFormatError):
                load_reflection_coverage(path, CODEC_PATH)

    def test_below_floor_decision_and_function_coverage_fail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = write_coverage(
                directory,
                {
                    "file": CODEC_PATH,
                    "lines": [
                        {
                            "line_number": 10,
                            "gcovr/decision": {
                                "type": "conditional",
                                "count_true": 1,
                                "count_false": 0,
                            },
                        },
                        {
                            "line_number": 11,
                            "gcovr/excluded": True,
                            "gcovr/decision": {"type": "switch", "count": 2},
                        },
                    ],
                    "functions": [function(execution_count=0)],
                },
            )
            coverage = load_reflection_coverage(path, CODEC_PATH)

        failures = gate_failures(coverage, 95.0, 100.0)
        self.assertEqual(len(failures), 2)
        self.assertIn("source-decision", failures[0])
        self.assertIn("function coverage", failures[1])


if __name__ == "__main__":
    unittest.main()
