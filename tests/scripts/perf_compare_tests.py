#!/usr/bin/env python3

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests" / "fixtures" / "performance"
MODULE_PATH = ROOT / "scripts" / "perf-compare.py"
SPEC = importlib.util.spec_from_file_location("scry_perf_compare", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
PERF = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PERF)


class PerfCompareTests(unittest.TestCase):
    def test_summary_matches_golden_scenarios(self) -> None:
        environment = PERF.read_json(FIXTURES / "environment.json")
        summary = PERF.summarized_document(
            environment, (FIXTURES / "raw").glob("*.json")
        )
        expected = json.loads(
            (FIXTURES / "scenarios.json").read_text(encoding="utf-8")
        )

        self.assertEqual(summary["schema_version"], PERF.SUMMARY_SCHEMA)
        self.assertEqual(summary["scenarios"], expected)

    def test_summary_rejects_missing_allocation_counter(self) -> None:
        environment = PERF.read_json(FIXTURES / "environment.json")
        timing = PERF.read_json(
            FIXTURES / "raw" / "scry_timing_benchmarks.json"
        )
        allocation = PERF.read_json(
            FIXTURES / "raw" / "scry_allocation_benchmarks.json"
        )
        del allocation["benchmarks"][0]["cpp_peak_live_requested_bytes"]

        with tempfile.TemporaryDirectory() as directory:
            raw_dir = Path(directory)
            PERF.write_json(raw_dir / "scry_timing_benchmarks.json", timing)
            PERF.write_json(raw_dir / "scry_allocation_benchmarks.json", allocation)
            with self.assertRaisesRegex(
                PERF.EvidenceError, "cpp_peak_live_requested_bytes"
            ):
                PERF.summarized_document(environment, raw_dir.glob("*.json"))

    def test_direct_comparison_inputs_remain_diagnostic(self) -> None:
        arguments = argparse.Namespace(
            manifest=None,
            base=[Path("base-summary.json")],
            head=[Path("head-summary.json")],
        )

        base, head, protocol, reasons = PERF.comparison_inputs(arguments)

        self.assertEqual(base, [Path("base-summary.json").resolve()])
        self.assertEqual(head, [Path("head-summary.json").resolve()])
        self.assertFalse(protocol["order_validated"])
        self.assertEqual(
            reasons, ["fresh-process A-B-B-A order was not manifest-validated"]
        )

    def test_semantic_counter_drift_is_rejected(self) -> None:
        scenarios = json.loads(
            (FIXTURES / "scenarios.json").read_text(encoding="utf-8")
        )
        base = scenarios[1]
        head = copy.deepcopy(base)
        head["counters"]["checksum_lo"]["samples"] = [35.0]

        with self.assertRaisesRegex(PERF.EvidenceError, "checksum_lo changed"):
            PERF.compare_scenario(base, head, 0)


if __name__ == "__main__":
    unittest.main()
