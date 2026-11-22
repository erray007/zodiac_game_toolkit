"""Tests for tools/check_bench.py, the gate over bench_types' JSON lines (spec 8.4).

Contract under test: ratios above 1.05 fail with exit code 1 and name every such operation, everything at or below passes
with exit code 0, and any input that is not a complete set of well-formed result lines fails with exit code 2: complete
means exactly the operations of the --expected list (bench_types --list), and a missing or unknown one is named; an unexpected
error in the check is also 2, so exit code 1 always means "too slow"."""

import io
import json
import re
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_bench


def line(op, ratio, new_ns=10.0, addon_ns=10.0):
    return json.dumps({"op": op, "new_ns": new_ns, "addon_ns": addon_ns, "ratio": ratio})


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
