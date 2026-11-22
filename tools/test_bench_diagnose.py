"""Tests for tools/bench_diagnose.py (Task 5b): the run schedule interleaves seeds, passes and unfixed processes; a run's results and
layout lines parse; the seed override check catches a seed that drew two keys and unfixed processes that drew one; the summary counts
slow processes per kind and whether the passes of a seed agree."""

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bench_diagnose

MACHINE = json.dumps({"machine": {"cpu": "Test CPU", "logical_cores": 4, "os": "Windows 10.0.26100.1"}})


def results(ratio):
    lines = [MACHINE]
    for op in ("dict_lookup_10", "dict_iterate_10"):
        lines.append(json.dumps({"op": op, "new_ns": 186.0 * ratio, "addon_ns": 186.0, "ratio": ratio if op == "dict_iterate_10" else 0.8}))
    return "\n".join(lines) + "\n"


def layout(op, key0, probe):
    table = {"table_key0": key0, "live": 10, "dictionary_mod4096": 0, "index_size": 32, "index_mod4096": 64, "segment0_mod4096": 128,
             "probe_total": probe, "probe_max": 1, "lines_touched": 12, "keys_array_mod4096": 256, "keys": []}
    return bench_diagnose.LAYOUT_PREFIX + json.dumps({"op": op, "engine": 0, "table": table})


def record(kind, seed, number, ratio, key0, probe=3):
    parsed = bench_diagnose.parse_run(results(ratio), "noise\n" + layout("dict_iterate_10", key0, probe) + "\n" + layout("dict_lookup_10", key0, 9) + "\n", "dict_iterate_10")
    parsed.update({"kind": kind, "seed": seed, "pass": number})
    return parsed


class BenchDiagnoseTests(unittest.TestCase):
    def test_the_schedule_interleaves_every_kind(self):
        runs = bench_diagnose.schedule(2, 2, 3)
        self.assertEqual(runs, [("seed", 1, 1), ("unfixed", None, 1), ("seed", 1, 2), ("seed", 2, 1), ("unfixed", None, 2), ("seed", 2, 2), ("unfixed", None, 3)])

    def test_a_run_parses_with_only_the_focus_layouts(self):
        parsed = record("seed", 1, 1, 1.7, "aa")
        self.assertEqual(parsed["machine"]["cpu"], "Test CPU")
        self.assertEqual(parsed["ops"]["dict_iterate_10"]["ratio"], 1.7)
        self.assertEqual(len(parsed["layouts"]), 1)
        self.assertEqual(parsed["layouts"][0]["probe_total"], 3)

    def test_a_run_without_the_focus_fails(self):
        with self.assertRaises(bench_diagnose.DiagnoseError):
            bench_diagnose.parse_run(results(1.0), "", "dict_iterate_10")

    def test_the_override_check(self):
        good = [record("seed", 1, 1, 0.8, "aa"), record("seed", 1, 2, 0.8, "aa"), record("unfixed", None, 1, 0.8, "bb"), record("unfixed", None, 2, 0.8, "cc")]
        self.assertEqual(bench_diagnose.check_override(good), [])
        bad = [record("seed", 1, 1, 0.8, "aa"), record("seed", 1, 2, 0.8, "dd"), record("unfixed", None, 1, 0.8, "bb"), record("unfixed", None, 2, 0.8, "bb")]
        problems = bench_diagnose.check_override(bad)
        self.assertEqual(len(problems), 2)
        self.assertIn("seed 1", problems[0])
        self.assertIn("leaked", problems[1])

    def test_the_summary_counts_slow_processes_and_pass_agreement(self):
        records = [record("seed", 1, 1, 1.7, "aa", 9), record("seed", 1, 2, 1.7, "aa", 9), record("seed", 2, 1, 0.8, "bb"), record("seed", 2, 2, 1.6, "bb"),
                   record("unfixed", None, 1, 0.8, "cc"), record("unfixed", None, 2, 0.79, "dd")]
        text = bench_diagnose.summary(records, "dict_iterate_10", 1.3)
        self.assertIn("seed: 3 of 4 processes slow", text)
        self.assertIn("unfixed: 0 of 2 processes slow", text)
        self.assertIn("1 slow in every pass, 1 slow in some passes only, 0 never slow", text)
        self.assertIn("probe_total: slow 7.0, normal 3.0", text)
        self.assertIn("machines: Test CPU / 4 cores / Windows 10.0.26100.1", text)


if __name__ == "__main__":
    unittest.main()
