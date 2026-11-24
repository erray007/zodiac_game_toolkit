"""Tests for tools/check_bench.py, the gate over bench_types' JSON lines (spec 8.4).

Contract under test: ratios above 1.05 fail with exit code 1 and name every such operation, everything at or below passes
with exit code 0, and any input that is not a complete set of well-formed result lines fails with exit code 2: complete
means exactly the operations of the --expected list (bench_types --list), and a missing or unknown one is named; an unexpected
error in the check is also 2, so exit code 1 always means "too slow"."""

import io
import json
import os
import re
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_bench


def line(op, ratio, new_ns=10.0, addon_ns=10.0, calibrate="time"):
    record = {"op": op, "new_ns": new_ns, "addon_ns": addon_ns, "ratio": ratio}
    if calibrate is not None:
        record.update({"calibrate": calibrate, "window_ms": 1.0, "zodiac_reps": 100, "addon_reps": 100})
    return json.dumps(record)


MACHINE = json.dumps({"machine": {"cpu": "AMD EPYC 7763 64-Core Processor", "logical_cores": 4, "os": "Windows 10.0.26100.4652"}})


class CheckBenchTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.path = Path(self._tmp.name) / "bench.json"

    def run_main(self, text, expected=None, machine=True):
        """Runs check_bench on text, with MACHINE appended unless machine is False (so line numbers stay those of text); expected is
        the op list bench_types --list would print (by default the ops in text)."""
        self.path.write_text(text + (MACHINE + "\n" if machine else ""), encoding="utf-8")
        if expected is None:
            expected = list(dict.fromkeys(re.findall(r'"op": "([^"]*)"', text)))
        ops = Path(self._tmp.name) / "ops.txt"
        ops.write_text("".join(name + "\n" for name in expected), encoding="utf-8")
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = check_bench.main([str(self.path), "--expected", str(ops)])
        return code, out.getvalue(), err.getvalue()

    def test_a_missing_op_fails_naming_it(self):
        code, _, err = self.run_main(line("string_find", 0.5) + "\n", expected=["string_find", "array_sort_int", "dict_insert_10"])
        self.assertEqual(code, 2)
        self.assertIn("array_sort_int", err)
        self.assertIn("dict_insert_10", err)
        self.assertNotIn("string_find", err)

    def test_an_unknown_op_fails_naming_it(self):
        code, _, err = self.run_main(line("string_find", 0.5) + "\n" + line("string_fnid", 0.5) + "\n", expected=["string_find"])
        self.assertEqual(code, 2)
        self.assertIn("string_fnid", err)

    def test_the_expected_list_is_required_and_readable(self):
        self.path.write_text(line("string_find", 0.5) + "\n", encoding="utf-8")
        err = io.StringIO()
        with redirect_stdout(io.StringIO()), redirect_stderr(err):
            code = check_bench.main([str(self.path), "--expected", str(Path(self._tmp.name) / "missing.txt")])
        self.assertEqual(code, 2)
        self.assertIn("missing.txt", err.getvalue())
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            check_bench.main([str(self.path)])
        self.assertEqual(raised.exception.code, 2)

    def test_an_empty_expected_list_fails(self):
        code, _, err = self.run_main(line("string_find", 0.5) + "\n", expected=[])
        self.assertEqual(code, 2)
        self.assertIn("no operations", err)

    def test_all_ratios_at_or_below_the_limit_pass(self):
        code, out, err = self.run_main("\n".join([line("string_find", 0.5), line("array_sort_int", 1.0), line("dict_insert_10", 1.05)]) + "\n")
        self.assertEqual(code, 0, err)
        for op in ("string_find", "array_sort_int", "dict_insert_10"):
            self.assertIn(op, out)
        self.assertEqual(err, "")

    def test_ratios_above_the_limit_fail_naming_every_such_op(self):
        code, out, err = self.run_main("\n".join([line("string_find", 1.2), line("array_sort_int", 0.9), line("dict_lookup_1000", 1.0501)]) + "\n")
        self.assertEqual(code, 1)
        self.assertIn("string_find", err)
        self.assertIn("dict_lookup_1000", err)
        self.assertNotIn("array_sort_int", err)

    def test_the_limit_is_five_percent(self):
        self.assertEqual(check_bench.LIMIT, 1.05)

    def test_malformed_json_fails(self):
        code, _, err = self.run_main(line("string_find", 0.5) + "\n{\"op\": \"array_find_int\", \"ratio\":\n")
        self.assertEqual(code, 2)
        self.assertIn("line 2", err)

    def test_missing_or_wrong_fields_fail(self):
        for bad in ('{"op": "x", "new_ns": 1, "addon_ns": 1}', '{"op": "x", "new_ns": 1, "addon_ns": 1, "ratio": "0.5"}', '{"op": "", "new_ns": 1, "addon_ns": 1, "ratio": 0.5}',
                '{"op": "x", "new_ns": 1, "addon_ns": 1, "ratio": NaN}', '{"op": "x", "new_ns": -1, "addon_ns": 1, "ratio": 0.5}', '{"op": "x", "new_ns": 1, "addon_ns": 1, "ratio": true}', '[1, 2]'):
            with self.subTest(bad=bad):
                code, _, err = self.run_main(bad + "\n")
                self.assertEqual(code, 2)
                self.assertIn("line 1", err)

    def test_a_repeated_op_fails(self):
        code, _, err = self.run_main(line("string_find", 0.5) + "\n" + line("string_find", 0.6) + "\n")
        self.assertEqual(code, 2)
        self.assertIn("string_find", err)

    def test_no_results_fail(self):
        code, _, err = self.run_main("\n\n")
        self.assertEqual(code, 2)
        self.assertIn("no results", err)

    def test_only_ratios_above_the_limit_give_exit_code_1(self):
        # A format failure wins over slow operations in the same file: the run is not a valid measurement.
        code, _, _ = self.run_main(line("string_find", 1.5) + "\n" + line("string_fnid", 1.5) + "\n", expected=["string_find"])
        self.assertEqual(code, 2)
        # An unexpected error in the check is 2, never Python's default 1, which would read as "too slow".
        original = check_bench.read
        def broken(text):
            raise RuntimeError("boom")
        check_bench.read = broken
        try:
            code, _, err = self.run_main(line("string_find", 0.5) + "\n")
        finally:
            check_bench.read = original
        self.assertEqual(code, 2)
        self.assertIn("internal error", err)

    def test_the_machine_is_printed_above_the_table(self):
        code, out, err = self.run_main(line("string_find", 0.5) + "\n")
        self.assertEqual(code, 0, err)
        first = out.splitlines()[0]
        self.assertEqual(first, "machine: AMD EPYC 7763 64-Core Processor, 4 logical cores, Windows 10.0.26100.4652")

    def test_the_machine_line_is_required_once_and_well_formed(self):
        code, _, err = self.run_main(line("string_find", 0.5) + "\n", machine=False)
        self.assertEqual(code, 2)
        self.assertIn("no machine line", err)
        code, _, err = self.run_main(MACHINE + "\n" + line("string_find", 0.5) + "\n")
        self.assertEqual(code, 2)
        self.assertIn("a second machine line", err)
        for bad in ('{"machine": {"cpu": "", "logical_cores": 4, "os": "x"}}', '{"machine": {"cpu": "x", "logical_cores": true, "os": "x"}}',
                '{"machine": {"cpu": "x", "logical_cores": -1, "os": "x"}}', '{"machine": {"cpu": "x", "logical_cores": 4}}', '{"machine": "x"}',
                '{"machine": {"cpu": "x", "logical_cores": 4, "os": "x"}, "op": "string_find"}'):
            with self.subTest(bad=bad):
                code, _, err = self.run_main(bad + "\n" + line("string_find", 0.5) + "\n", machine=False)
                self.assertEqual(code, 2)
                self.assertIn("line 1", err)

    def run_files(self, texts, expected, baselines=None):
        """Runs check_bench on several results files (and baseline files); each text gets MACHINE appended."""
        paths = []
        for index, text in enumerate(texts):
            path = Path(self._tmp.name) / f"run{index}.json"
            path.write_text(text + MACHINE + "\n", encoding="utf-8")
            paths.append(str(path))
        ops = Path(self._tmp.name) / "ops.txt"
        ops.write_text("".join(name + "\n" for name in expected), encoding="utf-8")
        argv = paths + ["--expected", str(ops)]
        if baselines is not None:
            base_paths = []
            for index, text in enumerate(baselines):
                path = Path(self._tmp.name) / f"main{index}.json"
                path.write_text(text, encoding="utf-8")
                base_paths.append(str(path))
            argv += ["--baseline"] + base_paths
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = check_bench.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_several_processes_gate_on_the_median_ratio(self):
        texts = [line("a", 1.2) + "\n" + line("b", 1.0) + "\n", line("a", 1.0) + "\n" + line("b", 1.06) + "\n", line("a", 1.01) + "\n" + line("b", 1.07) + "\n"]
        code, out, err = self.run_files(texts, ["a", "b"])
        self.assertEqual(code, 1)
        self.assertIn("b: ratio 1.060", err)
        self.assertNotIn("a: ratio", err)
        self.assertIn("median of 3 process(es)", out)
        code, _, err = self.run_files([line("a", 1.2) + "\n", line("a", 1.0) + "\n", line("a", 1.04) + "\n"], ["a"])
        self.assertEqual(code, 0, err)

    def test_processes_must_share_machine_and_order(self):
        other = json.dumps({"machine": {"cpu": "Other CPU", "logical_cores": 4, "os": "Windows 10.0.26100.4652"}})
        first = Path(self._tmp.name) / "one.json"
        second = Path(self._tmp.name) / "two.json"
        first.write_text(line("a", 1.0) + "\n" + MACHINE + "\n", encoding="utf-8")
        second.write_text(line("a", 1.0) + "\n" + other + "\n", encoding="utf-8")
        ops = Path(self._tmp.name) / "ops.txt"
        ops.write_text("a\n", encoding="utf-8")
        err = io.StringIO()
        with redirect_stdout(io.StringIO()), redirect_stderr(err):
            code = check_bench.main([str(first), str(second), "--expected", str(ops)])
        self.assertEqual(code, 2)
        self.assertIn("not on the first file's machine", err.getvalue())
        code, _, err = self.run_files([line("a", 1.0) + "\n" + line("b", 1.0) + "\n", line("b", 1.0) + "\n" + line("a", 1.0) + "\n"], ["a", "b"])
        self.assertEqual(code, 2)
        self.assertIn("another order", err)

    def test_both_calibration_modes_are_accepted_and_named(self):
        timed = json.dumps({"op": "a", "new_ns": 1.0, "addon_ns": 1.0, "ratio": 1.0, "calibrate": "time", "window_ms": 1.0, "zodiac_reps": 900, "addon_reps": 40})
        counted = json.dumps({"op": "a", "new_ns": 1.0, "addon_ns": 1.0, "ratio": 1.0, "calibrate": "count", "window_ms": 1.0, "zodiac_reps": 40, "addon_reps": 40})
        code, out, err = self.run_files([timed + "\n"], ["a"])
        self.assertEqual(code, 0, err)
        self.assertIn("calibration: time, window 1.0 ms", out)
        code, out, err = self.run_files([counted + "\n"], ["a"])
        self.assertEqual(code, 0, err)
        self.assertIn("calibration: count", out)

    def test_the_regression_gate_against_main(self):
        heads = [line("a", 0.5, new_ns=10.1) + "\n" + line("b", 0.5, new_ns=10.0) + "\n" + line("c", 0.5, new_ns=5.0) + "\n"] * 3
        # main lacks c (a new operation) and predates the machine line.
        mains = [line("a", 0.5, new_ns=10.0) + "\n" + line("b", 0.5, new_ns=10.0) + "\n"] * 3
        code, out, err = self.run_files(heads, ["a", "b", "c"], mains)
        self.assertEqual(code, 0, err)
        self.assertIn("not in main, not compared: c", out)
        self.assertIn("geometric mean head/main: 1.0050", out)
        # Past the per-operation limit.
        slow = [line("a", 0.5, new_ns=11.2) + "\n" + line("b", 0.5, new_ns=9.0) + "\n" + line("c", 0.5, new_ns=5.0) + "\n"] * 3
        code, _, err = self.run_files(slow, ["a", "b", "c"], mains)
        self.assertEqual(code, 1)
        self.assertIn("a: head/main 1.120", err)
        # Every operation a little slower: the geometric mean fails although no single one does.
        spread = [line("a", 0.5, new_ns=10.2) + "\n" + line("b", 0.5, new_ns=10.2) + "\n" + line("c", 0.5, new_ns=5.0) + "\n"] * 3
        code, _, err = self.run_files(spread, ["a", "b", "c"], mains)
        self.assertEqual(code, 1)
        self.assertIn("geometric mean 1.0200", err)
        # The baseline files pair with the results files.
        code, _, err = self.run_files(heads, ["a", "b", "c"], mains[:2])
        self.assertEqual(code, 2)
        self.assertIn("they are paired in order", err)
        # Each operation's best times, its head/main, the pair median (information) and its limit are printed.
        code, out, _ = self.run_files(heads, ["a", "b", "c"], mains)
        self.assertRegex(out, r"a\s+10\.00\s+10\.10\s+1\.010\s+1\.010\s+1\.04")
        self.assertIn("process speed factors (the add-on side's median over operations): head 1.000 1.000 1.000, main 1.000 1.000 1.000", out)

    def test_the_regression_gate_takes_each_side_s_best_process(self):
        mains = [line("a", 0.5, new_ns=10.0) + "\n" + line("b", 0.5, new_ns=10.0) + "\n"] * 3
        # Two of head's three processes in a slow mode on a: the pair median (1.3) would fail, the best (1.0) does not.
        heads = [line("a", 0.5, new_ns=13.0) + "\n" + line("b", 0.5) + "\n", line("a", 0.5, new_ns=13.0) + "\n" + line("b", 0.5) + "\n", line("a", 0.5, new_ns=10.0) + "\n" + line("b", 0.5) + "\n"]
        code, out, err = self.run_files(heads, ["a", "b"], mains)
        self.assertEqual(code, 0, err)
        self.assertRegex(out, r"a\s+10\.00\s+10\.00\s+1\.000\s+1\.300\s+1\.04")
        # A real slowdown is slow in every process and fails.
        slow = [line("a", 0.5, new_ns=11.0) + "\n" + line("b", 0.5) + "\n"] * 3
        code, _, err = self.run_files(slow, ["a", "b"], mains)
        self.assertEqual(code, 1)
        self.assertIn("a: head/main 1.100", err)

    def test_a_process_the_machine_ran_faster_is_corrected_by_the_add_on_side(self):
        ops = ["a", "b", "c", "d", "e"]
        def process(scale, a_ns=10.0):
            return "".join(line(op, 0.5, new_ns=(a_ns if op == "a" else 10.0) * scale, addon_ns=20.0 * scale) + "\n" for op in ops)
        heads = [process(1.0)] * 3
        # main's last process ran everything 10 % faster on both sides (the machine, not the code): uncorrected, its best would make
        # head read 1.11 slower on every operation.
        mains = [process(1.0), process(1.0), process(0.9)]
        code, out, err = self.run_files(heads, ops, mains)
        self.assertEqual(code, 0, err)
        self.assertIn("main 1.000 1.000 0.900", out)
        self.assertIn("geometric mean head/main: 1.0000", out)
        # One operation's own add-on slow mode does not move the factor (a median over operations).
        odd = "".join(line(op, 0.5, new_ns=10.0, addon_ns=60.0 if op == "b" else 20.0) + "\n" for op in ops)
        code, out, err = self.run_files([odd, process(1.0), process(1.0)], ops, [process(1.0)] * 3)
        self.assertEqual(code, 0, err)
        self.assertIn("head 1.000 1.000 1.000", out)

    def test_a_zero_head_time_names_the_op(self):
        mains = [line("a", 0.5) + "\n" + line("b", 0.5) + "\n"] * 3
        heads = [line("a", 0.5) + "\n" + line("b", 0.5, new_ns=0.0) + "\n"] * 3
        code, _, err = self.run_files(heads, ["a", "b"], mains)
        self.assertEqual(code, 2)
        self.assertIn("head gives b a time of 0 ns", err)
        self.assertNotIn("internal error", err)

    def test_the_results_files_must_agree_on_the_calibration_mode(self):
        timed = line("a", 1.0) + "\n"
        counted = line("a", 1.0, calibrate="count") + "\n"
        code, _, err = self.run_files([timed, counted, timed], ["a"])
        self.assertEqual(code, 2)
        self.assertIn("disagree on the calibration mode", err)
        mixed = line("a", 1.0) + "\n" + line("b", 1.0, calibrate="count") + "\n"
        code, _, err = self.run_files([mixed], ["a", "b"])
        self.assertEqual(code, 2)
        self.assertIn("mixed", err)

    def test_a_report_only_comparison_is_annotated_on_github(self):
        slow = [line("a", 0.5, new_ns=13.0) + "\n"] * 3
        old = [line("a", 0.5, new_ns=10.0, calibrate=None) + "\n"] * 3
        with mock.patch.dict(os.environ, {"GITHUB_ACTIONS": "true"}):
            code, out, _ = self.run_files(slow, ["a"], old)
        self.assertEqual(code, 0)
        self.assertIn("::warning title=Regression gate report-only::REGRESSION GATE REPORT-ONLY: main's bench records no calibration", out)
        with mock.patch.dict(os.environ):
            os.environ.pop("GITHUB_ACTIONS", None)
            code, out, _ = self.run_files(slow, ["a"], old)
        self.assertNotIn("::warning", out)

    def test_a_baseline_calibrated_otherwise_is_report_only(self):
        slow = [line("a", 0.5, new_ns=13.0) + "\n"] * 3
        # main's bench predates the calibrate field: report-only, loudly, and the slowdown does not fail the gate.
        old = [line("a", 0.5, new_ns=10.0, calibrate=None) + "\n"] * 3
        code, out, err = self.run_files(slow, ["a"], old)
        self.assertEqual(code, 0, err)
        self.assertIn("REGRESSION GATE REPORT-ONLY: main's bench records no calibration", out)
        self.assertIn("REGRESSION GATE REPORT-ONLY", err)
        self.assertIn("(report-only) slower than main", err)
        # main measured in count mode, head in time mode: report-only too.
        counted = [line("a", 0.5, new_ns=10.0, calibrate="count") + "\n"] * 3
        code, out, _ = self.run_files(slow, ["a"], counted)
        self.assertEqual(code, 0)
        self.assertIn("main's bench calibrated ['count'], head time", out)
        # The same mode on both sides gates.
        same = [line("a", 0.5, new_ns=10.0) + "\n"] * 3
        code, _, err = self.run_files(slow, ["a"], same)
        self.assertEqual(code, 1)
        self.assertNotIn("REPORT-ONLY", err)

    def test_a_zero_baseline_names_the_op(self):
        heads = [line("a", 0.5) + "\n" + line("b", 0.5) + "\n"] * 3
        zero = [line("a", 0.5, new_ns=0.0) + "\n" + line("b", 0.5) + "\n"] * 3
        code, _, err = self.run_files(heads, ["a", "b"], zero)
        self.assertEqual(code, 2)
        self.assertIn("gives a a time of 0 ns", err)
        self.assertNotIn("internal error", err)

    def test_per_op_limits_come_from_the_limits_file(self):
        limits = Path(self._tmp.name) / "limits.json"
        limits.write_text(json.dumps({"default": 1.04, "geomean": 1.10, "ops": {"a": {"limit": 1.12, "spread": 0.09, "runs": [1]}}}), encoding="utf-8")
        heads = [line("a", 0.5, new_ns=11.0) + "\n" + line("b", 0.5, new_ns=10.5) + "\n"] * 3
        mains = [line("a", 0.5, new_ns=10.0) + "\n" + line("b", 0.5, new_ns=10.0) + "\n"] * 3
        paths = []
        for name, texts in (("h", heads), ("m", mains)):
            for index, text in enumerate(texts):
                path = Path(self._tmp.name) / f"{name}{index}.json"
                path.write_text(text + (MACHINE + "\n" if name == "h" else ""), encoding="utf-8")
                paths.append(str(path))
        ops = Path(self._tmp.name) / "ops.txt"
        ops.write_text("a\nb\n", encoding="utf-8")
        err = io.StringIO()
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = check_bench.main(paths[:3] + ["--expected", str(ops), "--baseline"] + paths[3:] + ["--limits", str(limits)])
        # a (1.10) is within its own 1.12; b (1.05) is above the default 1.04.
        self.assertEqual(code, 1)
        self.assertIn("b: head/main 1.050 (limit 1.04)", err.getvalue())
        self.assertNotIn("a: head/main", err.getvalue())
        bad = Path(self._tmp.name) / "bad.json"
        bad.write_text(json.dumps({"default": 0.9, "geomean": 1.01, "ops": {}}), encoding="utf-8")
        with redirect_stdout(io.StringIO()), redirect_stderr(err):
            code = check_bench.main(paths[:3] + ["--expected", str(ops), "--baseline"] + paths[3:] + ["--limits", str(bad)])
        self.assertEqual(code, 2)

    def test_a_missing_file_fails(self):
        out = io.StringIO()
        err = io.StringIO()
        ops = Path(self._tmp.name) / "ops.txt"
        ops.write_text("string_find\n", encoding="utf-8")
        with redirect_stdout(out), redirect_stderr(err):
            code = check_bench.main([str(self.path), "--expected", str(ops)])
        self.assertEqual(code, 2)
        self.assertIn(str(self.path), err.getvalue())


if __name__ == "__main__":
    unittest.main()
